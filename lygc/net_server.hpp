#ifndef net_server_hpp
#define net_server_hpp

#include <atomic>
#include <list>
#include <mutex>
#include <unordered_map>

#include "lymsg_protocol.hpp"
#include "lyserver_config.hpp"
#include "zbf/log_utils.hpp"
#include "zbf/timer_helper.hpp"

namespace lygc {

using zbf::object_tracker;
using zbf::log_utils;
using zbf::LogLevel;
using zbf::tcpsock_user;
using zbf::tcpsock_server;
using zbf::tcpsock_ha_asynclt;
using zbf::async_client_manager;

enum RespType { SYNC_RESPONSE = 0, NO_RESPONSE, ASYNC_RESPONSE = 101 };
typedef uint64_t request_id_t;

class UserHandler {
public:
    virtual request_id_t onRequest(const lymsg_header* reqHeader, const std::string& reqData, std::string& syncRespData) = 0;
};

class Responser {
public:
    virtual void onResponse(const lymsg_header* respHeader, const std::string& respData) = 0;
};

// NetUser
class NetServer;
class NetUser : public tcpsock_user {
public:
    NetUser(NetServer* base, uint16_t origin) : tcpsock_user(), _baseServer(base), _baseOrigin(origin), _handler(nullptr) { }

    virtual void onConnect() override { }

    virtual void onDisconnect() override;

    virtual void onRecvMsg(const tcp_message* msg) override {
        lymsg_header reqHeader;
        std::string reqData;
        int rc = lymsg_helper::unpackMsg(msg, reqHeader, reqData);
        if (!rc) {
            onRequest(&reqHeader, reqData);
        } else {
            LOG_ERR_MSG("unpackMsg fail: msg=%s, user=%s", msg->desc().c_str(), desc().c_str());
        }
    }

    virtual void onRequest(const lymsg_header* reqHeader, const std::string& reqData);

    virtual tcp_message* createResponse(const lymsg_header* reqHeader, const std::string& respData) {
        lymsg_header respHeader;
        memset(&respHeader, 0, sizeof(respHeader));
        respHeader.origin = _baseOrigin;
        respHeader.type = reqHeader->type | LYMSG_TYPE_RESP;
        respHeader.serial = reqHeader->serial;
        respHeader.param = 0;
        return lymsg_helper::packMsg(&respHeader, respData);
    }

    void postMsg(tcp_message* msg) {
        int rc = tcpsock_user::post(msg);
        if (rc) {
            LOG_ERR_MSG("post msg fail: msg=%s, user=%s, rc=%d", msg->desc().c_str(), desc().c_str(), rc);
            delete msg;
        }
    }

protected:
    NetServer* _baseServer;
    uint16_t   _baseOrigin;
    UserHandler* _handler;
};

// NetClient(HA)
class NetClient : public tcpsock_ha_asynclt {
public:
    using Handler = std::function<void(const lymsg_header* respHeader, const std::string& respData)>;

    NetClient(const char* name, uint16_t localOrigin) : tcpsock_ha_asynclt(name) {
        _localOrigin = localOrigin;
        _defaultResponser = nullptr;
    }

    void attachResponser(Responser* responser) {
        _defaultResponser = responser;
    }

    int request(lymsg_header* reqHeader, const std::string& reqData, Handler handler = nullptr) {
        if (!reqHeader) return -1;
        reqHeader->origin = _localOrigin;
        zbf::tcp_message* msg = lymsg_helper::packMsg(reqHeader, reqData);
        int rc = tcpsock_ha_asynclt::post(msg);
        if (zbf::POST_SUCCESS == rc) {
            if (handler) {
                std::lock_guard<std::mutex> lock(_lockHandlers);
                _msgHandlers.insert(std::make_pair(reqHeader->serial, handler));
            }
        } else {
            delete msg;
        }
        return rc;
    }

    virtual void onResponse(const tcp_message* msg) override {
        lymsg_header respHeader;
        std::string respData;
        int rc = lymsg_helper::unpackMsg(msg, respHeader, respData);
        if (!rc) {
            Handler handler = nullptr;
            {
                std::lock_guard<std::mutex> lock(_lockHandlers);
                auto it = _msgHandlers.find(respHeader.serial);
                if (it != _msgHandlers.end()) {
                    handler = it->second;
                    _msgHandlers.erase(it);
                }
            }
            
            if (handler) {
                handler(&respHeader, respData);
            } else if (_defaultResponser) {
                _defaultResponser->onResponse(&respHeader, respData);
            } else {
                LOG_ERR_MSG("no responser found: resp msg=%s", LYMSG_DESC(&respHeader).c_str());
            }
        } else {
            LOG_ERR_MSG("unpackMsg fail: msg=%s", msg->desc().c_str());
        }
    }

private:
    std::unordered_map<request_id_t, Handler> _msgHandlers;
    std::mutex _lockHandlers;
    uint16_t   _localOrigin;
    Responser* _defaultResponser;
};

struct ReqContext : object_tracker<ReqContext> {
    NetUser* user;
    lygc::lymsg_header* reqHeader;
    std::string reqData;
    
    ReqContext(NetUser* u, const lymsg_header* header, const std::string& data): user(u) {
        reqHeader = lymsg_helper::copyHeader(header);
        reqData = data;
    }
    ~ReqContext() {
        if (reqHeader) delete reqHeader;
    }
};

// NetServer
class NetServer : public tcpsock_server {
public:
    NetServer(const std::string& name, unsigned short origin, const std::string& host, unsigned short port, LogLevel logLevel = LogLevel::Info)
    : NetServer(ServerConfig(name, origin, host, port), logLevel) {
    }

    NetServer(const ServerConfig& conf, LogLevel logLevel = LogLevel::Info) : tcpsock_server(new lymsg_protocol),
    _config(conf), _reqIdCreator(RespType::ASYNC_RESPONSE), _asynCltMgr(new lymsg_protocol), _timerHelper(zbf::TickUnit::TenSecond) {
        std::string log_path = zbf::log_utils::createLogPath();
        log_utils::open(log_path.c_str(), logLevel);
        LOG_MSG(LogLevel::Info, "NetServer create(%s)", desc().c_str());
    }

    virtual ~NetServer() {
        log_utils::close();
    }

    NetClient* addAsynClient(const ServerConfig& target) {
        NetClient* client = new NetClient(target.name.c_str(), _config.origin);
        client->addConnect(target.host.c_str(), target.port, _asynCltMgr.getProtocol());
        _asynCltMgr.manageClient(client);
        return client;
    }

    NetClient* addAsynClient(const ServerConfGroup& group) {
        if (group.empty()) return nullptr;
        ServerConfig front = group.front();
        NetClient* client = new NetClient(front.name.c_str(), _config.origin);
        for (auto conf : group) {
            client->addConnect(conf.host.c_str(), conf.port, _asynCltMgr.getProtocol());
        }
        _asynCltMgr.manageClient(client);
        return client;
    }

    int open() {
        return tcpsock_server::open(_config.host.c_str(), _config.port);
    }

    void start(int serverWorkers = 8, int cltMgrWorkers = 0) {
        tcpsock_server::start(serverWorkers);

        _startAsyncMgr = (cltMgrWorkers > 0);
        if (_startAsyncMgr) {
            _asynCltMgr.start(cltMgrWorkers);
        }

        _timerHelper.start(tcpsock_server::ThdIdBase+5);
        _timerHelper.addTimerTask(5*60/10, showStatus, this); // 5 mins
    }

    virtual void serveUtilStop() override {
        tcpsock_server::serveUtilStop();

        _timerHelper.stop();
        if (_startAsyncMgr) {
            _asynCltMgr.stop(true);
        }
        for (auto item : _userHandlers) {
            UserHandler* handler = item.second;
            delete handler;
        }
        _userHandlers.clear();
    }

    virtual tcpsock_user* createUser() override {
        return new NetUser(this, origin());
    }

    static bool showStatus(void* param) {
        NetServer* server = (NetServer*)param;
        server->status();
        if (server->_startAsyncMgr) {
            server->_asynCltMgr.status();
        }
        zbf::logMemTrackStat(false);
        return true;
    }

public:
    void registerUserHandler(unsigned short origin, UserHandler* handler) {
        std::lock_guard<std::mutex> lock(_lockUserHandlers);
        unsigned int key = origin;
        _userHandlers[key] = handler;
    }

    UserHandler* getUserHandler(unsigned short origin) {
        std::lock_guard<std::mutex> lock(_lockUserHandlers);
        unsigned int key = origin;
        UserHandler* handler = nullptr;
        auto it = _userHandlers.find(key);
        if (it != _userHandlers.end()) {
            handler = it->second;
        }
        return handler;
    }

    void saveReqContext(request_id_t reqId, NetUser* user, const lymsg_header* reqHeader, const std::string& reqData) {
        std::lock_guard<std::mutex> lock(_lockReqContext);
        ReqContext* ctx = new ReqContext(user, reqHeader, reqData);
        _reqContext.insert(std::pair<request_id_t, ReqContext*>(reqId, ctx));
        _user2ReqIds[user].push_back(reqId);
        LOG_MSG(LogLevel::Trace, "%s, id=%lu, req=%s", __FUNCTION__, reqId, LYMSG_DESC(ctx->reqHeader).c_str());
    }

    int response(request_id_t reqId, const std::string& respData) {
        int rc = 0;
        std::lock_guard<std::mutex> lock(_lockReqContext);
        auto it = _reqContext.find(reqId);
        if (it != _reqContext.end()) {
            ReqContext* ctx = it->second;
            NetUser* user = ctx->user;
            tcp_message* resp = user->createResponse(ctx->reqHeader, respData);
            LOG_MSG(LogLevel::Trace, "%s success, id=%lu, req=%s, respSz=%lu", __FUNCTION__, reqId, 
                LYMSG_DESC(ctx->reqHeader).c_str(), respData.size());
            user->postMsg(resp);

            _user2ReqIds[user].remove(reqId);
            _reqContext.erase(it);
            delete ctx;
        } else {
            rc = -1;
            LOG_ERR_MSG("%s fail, id=%lu", __FUNCTION__, reqId);
        }
        return rc;
    }

public:
    request_id_t genRequestId() {
        request_id_t reqId = _reqIdCreator.fetch_add(1, std::memory_order_seq_cst);
        return reqId;
    }

    void onUserDisconnect(NetUser* user) {
        // remove pendding ReqContext
        std::lock_guard<std::mutex> lock(_lockReqContext);
        auto it = _user2ReqIds.find(user);
        int pendding = 0;
        if (it != _user2ReqIds.end()) {
            pendding = it->second.size();
            for (request_id_t id : it->second) {
                auto result = _reqContext.find(id);
                if (result != _reqContext.end()) {
                    delete result->second;
                    _reqContext.erase(result);
                }
            }
            _user2ReqIds.erase(it);
        }
        LOG_MSG(LogLevel::Debug, "onUserDisconnect(%s), pendding request=%lu", user->desc().c_str(), pendding);
    }

    uint16_t origin() {
        return _config.origin;
    }

    std::string desc() {
        return _config.toJsonStr();
    }

protected:
    ServerConfig _config;
    std::unordered_map<unsigned int, UserHandler*> _userHandlers;
    std::mutex _lockUserHandlers;

    std::unordered_map<request_id_t, ReqContext*> _reqContext;
    std::unordered_map<NetUser*, std::list<request_id_t> > _user2ReqIds;
    std::mutex _lockReqContext; // lock _reqContext & _user2ReqIds
    std::atomic<request_id_t> _reqIdCreator;

    bool _startAsyncMgr{false};
    async_client_manager _asynCltMgr;
    zbf::timer_helper _timerHelper;
};

// NetUser
void NetUser::onRequest(const lymsg_header* reqHeader, const std::string& reqData) {
    if (!_handler) {
        _handler = _baseServer->getUserHandler(reqHeader->origin);
    }
    if (_handler) {
        std::string respData;
        request_id_t reqId = _handler->onRequest(reqHeader, reqData, respData);
        if (reqId == RespType::SYNC_RESPONSE) {
            tcp_message* resp = createResponse(reqHeader, respData);
            postMsg(resp);
        } else if (reqId == RespType::NO_RESPONSE) {
            tcp_message* ack = createResponse(reqHeader, std::string());
            postMsg(ack);
        } else {
            // save request context
            _baseServer->saveReqContext(reqId, this, reqHeader, reqData);
        }
    } else {
        LOG_ERR_MSG("no matched handler: msg=%s", LYMSG_DESC(reqHeader).c_str());
    }
}

void NetUser::onDisconnect() {
    _baseServer->onUserDisconnect(this);
}

}

#endif
