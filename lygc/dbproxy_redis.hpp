#pragma once

#include <array>
#include <string>
#include <vector>

#include "dbproxy_base.hpp"
#include "wjp/redis_client.hpp"

namespace lygc {

using wjp::redis_client_req;
using wjp::redis_client_resp;
using wjp::redis_client_pool;
using wjp::redis_client_pool_listener;
using wjp::redis_request_id_t;

struct RedisPoolConf {
    wjp::redis_client_connect_param param;
    int pool_size;
};

typedef std::array<redis_client_pool*, DBProxyBase::MAX_DB_SLOTS> RedisPoolArray;

class DBProxyRedis;

class DBProxyRedisUser : public DBProxyUserBase {
public:
    using DBProxyUserBase::DBProxyUserBase;
    virtual void onRecvMsg(const tcp_message* msg) override;
};

class DBProxyRedis : public DBProxyBase, public redis_client_pool_listener {
public:
    using DBProxyBase::DBProxyBase;

    virtual ~DBProxyRedis() = default;

    void start(const std::vector<RedisPoolConf>& pools, int serverWorkers = 1) {
        _poolConfs = pools;
        initPools();
        tcpsock_server::start(serverWorkers);
        LOG_MSG(LogLevel::Info, "DBProxyRedis(%s) start", desc().c_str());
    }

    tcpsock_user* createUser() override;

    void submit(std::unique_ptr<redis_client_req> req, const lymsg_header* reqHeader, tcpsock_user* user) {
        uint8_t slot = req->db_slot;
        if (slot >= MAX_DB_SLOTS) {
            LOG_ERR_MSG("invalid db slot=%u, req=%s", slot, req->desc().c_str());
            return;
        }
        redis_client_pool* pool = _pools[slot];

        if (pool) {
            auto desc = req->desc();
            redis_request_id_t dbReqId = pool->submit(std::move(req));
            if (dbReqId != redis_client_pool::PoolIsBusy) {
                LOG_MSG(LogLevel::Trace, "redis request, req=%s", desc.c_str());
                savePenddingReq(dbReqId, reqHeader, user, slot);
            } else {
                LOG_ERR_MSG("submit fail, req=%s", desc.c_str());
            }
        } else {
            LOG_ERR_MSG("database not found, slot=%u, req=%s", slot, req->desc().c_str());
        }
    }

    virtual void onResponse(const redis_client_req* req, const redis_client_resp* resp) override {
        onResponseBase(req->req_id, resp->serialize(), req->db_slot);
    }

protected:
    // hiredis has no global init/cleanup
    void initDriver() override {}
    void cleanupDriver() override {}

    void initPools() override {
        for (const auto& conf : _poolConfs) {
            if (conf.param.DBSlotNo >= MAX_DB_SLOTS) {
                LOG_ERR_MSG("initPools: invalid DBSlotNo=%u, skip", conf.param.DBSlotNo);
                continue;
            }
            redis_client_pool* pool = new redis_client_pool(conf.param, this);
            pool->start(conf.pool_size);
            _pools[conf.param.DBSlotNo] = pool;
            auto poolCtx = std::make_unique<DBCltPoolCtx>();
            registerPoolCtx(conf.param.DBSlotNo, std::move(poolCtx));
        }
    }

    void cleanupPools() override {
        RedisPoolArray poolCopy = _pools;
        _pools.fill(nullptr);
        for (auto* pool : poolCopy) {
            if (pool) {
                pool->stop();
                pool->serveUtilStop();
                delete pool;
            }
        }
        LOG_MSG(LogLevel::Trace, "DBProxyRedis, all pools stopped");
    }

private:
    RedisPoolArray _pools{};
    std::vector<RedisPoolConf> _poolConfs;
};

inline tcpsock_user* DBProxyRedis::createUser() {
    return new DBProxyRedisUser(this, _config.origin);
}

inline void DBProxyRedisUser::onRecvMsg(const tcp_message* msg) {
    lymsg_header reqHeader;
    std::string reqData;
    int rc = lymsg_helper::unpackMsg(msg, reqHeader, reqData);
    if (rc) {
        LOG_ERR_MSG("unpackMsg fail: msg=%s, user=%s", msg->desc().c_str(), desc().c_str());
        return;
    }
    if (reqHeader.type == LYMSG_TYPE_DB_REDIS) {
        auto req = redis_client_req::deserialize(reqData);
        static_cast<DBProxyRedis*>(_proxy)->submit(std::move(req), &reqHeader, this);
    } else {
        LOG_ERR_MSG("unexpected msg type=%u, user=%s", reqHeader.type, desc().c_str());
    }
}

}  // namespace lygc
