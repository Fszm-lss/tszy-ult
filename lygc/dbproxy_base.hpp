#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "lymsg_protocol.hpp"
#include "lyserver_config.hpp"
#include "zbf/log_utils.hpp"

namespace lygc {

using zbf::LogLevel;
using zbf::log_utils;
using zbf::tcpsock_user;
using zbf::tcpsock_server;

class DBProxyBase;

// Per-connection user session
class DBProxyUserBase : public tcpsock_user {
public:
    DBProxyUserBase(DBProxyBase* proxy, uint16_t origin) : tcpsock_user(), _proxy(proxy), _origin(origin) {}

    virtual void onConnect() override {}
    virtual void onDisconnect() override;
    // onRecvMsg is implemented by each DB-type-specific subclass

    tcp_message* createResponse(const lymsg_header* reqHeader, const std::string& respData) {
        lymsg_header respHeader;
        memset(&respHeader, 0, sizeof(respHeader));
        respHeader.origin = _origin;
        respHeader.type = reqHeader->type | LYMSG_TYPE_RESP;
        respHeader.serial = reqHeader->serial;
        return lymsg_helper::packMsg(&respHeader, respData);
    }

protected:
    DBProxyBase* _proxy;
    uint16_t _origin;
};

// Holds request context until the async DB response arrives
struct PenddingReq : zbf::object_tracker<PenddingReq> {
    std::weak_ptr<tcpsock_user> userWeak;
    lymsg_header reqHeader;

    PenddingReq(std::weak_ptr<tcpsock_user> u, const lymsg_header* header) : userWeak(u) {
        memcpy(&reqHeader, header, sizeof(lymsg_header));
    }

    PenddingReq(const PenddingReq&) = delete;
    PenddingReq& operator=(const PenddingReq&) = delete;
};

// Per-database sharded pendding-request storage
struct DBCltPoolCtx {
    static constexpr size_t SHARD_BITS = 4;
    static constexpr size_t SHARD_COUNT = 1 << SHARD_BITS;

    struct Shard {
        std::mutex lock;
        std::unordered_map<uint64_t, std::unique_ptr<PenddingReq>> penddingReqs;
    };
    std::array<Shard, SHARD_COUNT> shards;

    static size_t shardIdx(uint64_t dbReqId) {
        return dbReqId & (SHARD_COUNT - 1);
    }
};


// Base proxy: common infrastructure for all DB types
class DBProxyBase : public tcpsock_server {
public:
    static constexpr size_t MAX_DB_SLOTS = 64;
    // Forwarding constructors
    DBProxyBase(const std::string& name, unsigned short origin, const std::string& host, unsigned short port, LogLevel logLevel = LogLevel::Info)
        : DBProxyBase(ServerConfig(name, origin, host, port), logLevel) {}

    DBProxyBase(const ServerConfig& conf, LogLevel logLevel = LogLevel::Info) : tcpsock_server(new lymsg_protocol), _config(conf) {
        std::string log_path = zbf::log_utils::createLogPath();
        log_utils::open(log_path.c_str(), logLevel);
        LOG_MSG(LogLevel::Info, "DBProxy(%s) create", desc().c_str());
    }

    virtual ~DBProxyBase() {
        log_utils::close();
    }

    int open() {
        int rc = tcpsock_server::open(_config.host.c_str(), _config.port);
        if (!rc) {
            initDriver();
        }
        return rc;
    }

    virtual void close() override {
        tcpsock_server::close();
        cleanupDriver();
    }

    virtual void serveUtilStop() override {
        tcpsock_server::serveUtilStop();

        // Subclass stops its pools first (workers may still reference contexts)
        cleanupPools();

        // Release pool contexts (unique_ptr auto-destroys PenddingReq + DBCltPoolCtx)
        for (auto& ctx : _poolCtx) ctx.reset();
        LOG_MSG(LogLevel::Trace, "DBProxy(%s), all pool contexts released", desc().c_str());
        LOG_MSG(LogLevel::Info,  "DBProxy(%s) stop", desc().c_str());
    }

    virtual tcpsock_user* createUser() override = 0;

    void cleanupUserPending(tcpsock_user* user) {
        auto userShared = user->shared_from_this();
        for (auto& poolCtx : _poolCtx) {
            if (!poolCtx) continue;
            auto& shards = poolCtx->shards;
            for (auto& shard : shards) {
                std::lock_guard<std::mutex> shardLock(shard.lock);
                auto it = shard.penddingReqs.begin();
                while (it != shard.penddingReqs.end()) {
                    auto pendingUser = it->second->userWeak.lock();
                    if (!pendingUser || pendingUser == userShared) {
                        it = shard.penddingReqs.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }
    }

    std::string desc() {
        return _config.toJsonStr();
    }

protected:
    // Match an async DB response to its pending request and forward to the client
    void onResponseBase(uint64_t dbReqId, const std::string& respData, uint8_t db_slot) {
        if (db_slot >= MAX_DB_SLOTS) {
            LOG_ERR_MSG("onResponseBase: invalid db_slot=%u, dbReqId=%lu", db_slot, dbReqId);
            return;
        }
        auto& poolCtx = _poolCtx[db_slot];
        if (!poolCtx) {
            LOG_ERR_MSG("onResponseBase: context not found, slot=%u, dbReqId=%lu", db_slot, dbReqId);
            return;
        }

        std::unique_ptr<PenddingReq> pendReq;
        {
            auto& shard = poolCtx->shards[DBCltPoolCtx::shardIdx(dbReqId)];
            std::lock_guard<std::mutex> lock(shard.lock);
            auto it = shard.penddingReqs.find(dbReqId);
            if (it != shard.penddingReqs.end()) {
                pendReq = std::move(it->second);
                shard.penddingReqs.erase(it);
            }
        }
        if (!pendReq) {
            LOG_ERR_MSG("onResponseBase: pendding req not found, dbReqId=%lu", dbReqId);
            return;
        }

        auto user = std::dynamic_pointer_cast<DBProxyUserBase>(pendReq->userWeak.lock());
        if (!user) {
            LOG_MSG(LogLevel::Trace, "onResponseBase: user gone, dbReqId=%lu, slot=%u", dbReqId, db_slot);
            return;
        }

        user->post(std::unique_ptr<tcp_message>(user->createResponse(&pendReq->reqHeader, respData)));
        LOG_MSG(LogLevel::Trace, "onResponseBase: slot=%u, dbReqId=%ld, user=%s", db_slot, dbReqId, user->desc().c_str());
    }

    // Save a pending request so it can be matched when the async response arrives
    void savePenddingReq(uint64_t dbReqId, const lymsg_header* reqHeader, tcpsock_user* user, uint8_t db_slot) {
        if (db_slot >= MAX_DB_SLOTS) {
            LOG_ERR_MSG("savePenddingReq: invalid db_slot=%u", db_slot);
            return;
        }
        auto& poolCtx = _poolCtx[db_slot];
        if (!poolCtx) {
            LOG_ERR_MSG("savePenddingReq: context not found, slot=%u", db_slot);
            return;
        }
        auto userShared = user->shared_from_this();
        auto pendReq = std::make_unique<PenddingReq>(userShared, reqHeader);
        auto& shard = poolCtx->shards[DBCltPoolCtx::shardIdx(dbReqId)];
        std::lock_guard<std::mutex> lock(shard.lock);
        shard.penddingReqs.insert(std::make_pair(dbReqId, std::move(pendReq)));
    }

    // Register a pool context (called by subclass during initPools)
    void registerPoolCtx(uint8_t slot, std::unique_ptr<DBCltPoolCtx> ctx) {
        if (slot >= MAX_DB_SLOTS) {
            LOG_ERR_MSG("registerPoolCtx: invalid slot=%u", slot);
            return;
        }
        _poolCtx[slot] = std::move(ctx);
    }

    // Virtual hooks, implemented by subclass
    virtual void initDriver() = 0;
    virtual void cleanupDriver() = 0;
    virtual void initPools() = 0;
    virtual void cleanupPools() = 0;

protected:
    ServerConfig _config;
    std::array<std::unique_ptr<DBCltPoolCtx>, MAX_DB_SLOTS> _poolCtx{};
};

// Out-of-line inline definition (requires DBProxyBase to be complete)
inline void DBProxyUserBase::onDisconnect() {
    _proxy->cleanupUserPending(this);
}

}  // namespace lygc
