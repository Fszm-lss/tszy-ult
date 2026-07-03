#pragma once

#include <array>
#include <string>
#include <vector>

#include "dbproxy_base.hpp"
#include "wjp/mysql_client.hpp"

namespace lygc {

using wjp::mysql_client_req;
using wjp::mysql_client_resp;
using wjp::mysql_client_pool;
using wjp::mysql_client_pool_listener;
using wjp::mysql_request_id_t;

struct MySQLPoolConf {
    wjp::mysql_client_connect_param param;
    int pool_size;
};

typedef std::array<mysql_client_pool*, DBProxyBase::MAX_DB_SLOTS> MysqlPoolArray;

class DBProxyMySQL;

class DBProxyMySQLUser : public DBProxyUserBase {
public:
    using DBProxyUserBase::DBProxyUserBase;
    virtual void onRecvMsg(const tcp_message* msg) override;
};

class DBProxyMySQL : public DBProxyBase, public mysql_client_pool_listener {
public:
    using DBProxyBase::DBProxyBase;

    virtual ~DBProxyMySQL() = default;

    void start(const std::vector<MySQLPoolConf>& pools, int serverWorkers = 1) {
        _poolConfs = pools;
        initPools();
        tcpsock_server::start(serverWorkers);
        LOG_MSG(LogLevel::Info, "DBProxyMySQL(%s) start", desc().c_str());
    }

    tcpsock_user* createUser() override;

    void submit(std::unique_ptr<mysql_client_req> req, const lymsg_header* reqHeader, tcpsock_user* user) {
        uint8_t slot = req->db_slot;
        if (slot >= MAX_DB_SLOTS) {
            LOG_ERR_MSG("invalid db slot=%u, req=%s", slot, req->desc().c_str());
            return;
        }
        mysql_client_pool* pool = _pools[slot];

        if (pool) {
            auto desc = req->desc();
            mysql_request_id_t dbReqId = pool->submit(std::move(req));
            if (dbReqId != mysql_client_pool::PoolIsBusy) {
                LOG_MSG(LogLevel::Trace, "mysql request, req=%s", desc.c_str());
                savePenddingReq(dbReqId, reqHeader, user, slot);
            } else {
                LOG_ERR_MSG("submit fail, req=%s", desc.c_str());
            }
        } else {
            LOG_ERR_MSG("database not found, slot=%u, req=%s", slot, req->desc().c_str());
        }
    }

    // Pool listener callback
    virtual void onResponse(const mysql_client_req* req, const mysql_client_resp* resp) override {
        onResponseBase(req->req_id, resp->serialize(), req->db_slot);
    }

protected:
    void initDriver() override {
        wjp::mysql_client_init();
    }

    void cleanupDriver() override {
        wjp::mysql_client_cleanup();
    }

    void initPools() override {
        for (const auto& conf : _poolConfs) {
            if (conf.param.DBSlotNo >= MAX_DB_SLOTS) {
                LOG_ERR_MSG("initPools: invalid DBSlotNo=%u, skip", conf.param.DBSlotNo);
                continue;
            }
            mysql_client_pool* pool = new mysql_client_pool(conf.param, this);
            pool->start(conf.pool_size);
            _pools[conf.param.DBSlotNo] = pool;
            auto poolCtx = std::make_unique<DBCltPoolCtx>();
            registerPoolCtx(conf.param.DBSlotNo, std::move(poolCtx));
        }
    }

    void cleanupPools() override {
        MysqlPoolArray poolCopy = _pools;
        _pools.fill(nullptr);
        for (auto* pool : poolCopy) {
            if (pool) {
                pool->stop();
                pool->serveUtilStop();
                delete pool;
            }
        }
        LOG_MSG(LogLevel::Trace, "DBProxyMySQL, all pools stopped");
    }

private:
    MysqlPoolArray _pools{};
    std::vector<MySQLPoolConf> _poolConfs;
};

// Out-of-line inline definitions (both classes must be complete)
inline tcpsock_user* DBProxyMySQL::createUser() {
    return new DBProxyMySQLUser(this, _config.origin);
}

inline void DBProxyMySQLUser::onRecvMsg(const tcp_message* msg) {
    lymsg_header reqHeader;
    std::string reqData;
    int rc = lymsg_helper::unpackMsg(msg, reqHeader, reqData);
    if (rc) {
        LOG_ERR_MSG("unpackMsg fail: msg=%s, user=%s", msg->desc().c_str(), desc().c_str());
        return;
    }
    if (reqHeader.type == LYMSG_TYPE_DB_MYSQL) {
        auto req = mysql_client_req::deserialize(reqData);
        static_cast<DBProxyMySQL*>(_proxy)->submit(std::move(req), &reqHeader, this);
    } else {
        LOG_ERR_MSG("unexpected msg type=%u, user=%s", reqHeader.type, desc().c_str());
    }
}

}  // namespace lygc
