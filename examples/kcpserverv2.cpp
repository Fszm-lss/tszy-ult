
#include "zbf/socket_kcp_v2.hpp"
#include "zbf/signal_utils.hpp"
#include "httplib.h"

using namespace zbf;

kcpsock_server*  g_kcpServer = nullptr;
httplib::Server* g_httpServer = nullptr;

class UserParamMgr {
public:
    void init() {
        for (int i = 1001; i <= 1000000; ++i) {
            identColl.push_back(i);
        }
        for (int i = 11001; i <= 60000; ++i) {
            portColl.push_back(i);
        }
    }

    bool pop(unsigned int& ident, unsigned short& port) {
        std::lock_guard<std::mutex> lock(_lockColl);
        if (identColl.empty() || portColl.empty()) return false;
        ident = identColl.front();
        port = portColl.front();
        identColl.pop_front();
        portColl.pop_front();
        return true;
    }

    void push(unsigned int ident, unsigned short port) {
        std::lock_guard<std::mutex> lock(_lockColl);
        identColl.push_back(ident);
        portColl.push_back(port);
    }

public:
    std::list<unsigned int> identColl;
    std::list<unsigned short> portColl;
    std::mutex  _lockColl;
};

UserParamMgr* g_upMgr = nullptr;

class listener : public kcpsock_listener {
public:
	void onRecvMsg(std::shared_ptr<kcp_user> user, std::unique_ptr<tcp_message> msg) override {
        std::string body(msg->data.begin() + user->getProtocol()->headerSize(), msg->data.end());
        LOG_MSG(LogLevel::Info, "onRecvMsg(%s), body=%s", user->desc().c_str(), body.c_str());
        // echo with type 0x42
        tcp_message* resp = dynamic_cast<default_msg_proto*>(user->getProtocol())->genMessage(0x42, body);
        if (resp && user->post(std::unique_ptr<tcp_message>(resp))) {
            // post failed, resp already moved and will be cleaned up
        }
    }

    void onDisconnect(std::shared_ptr<kcp_user> user) override {
        LOG_MSG(LogLevel::Info, "onDisconnect(%s), conv=%d, peerPort=%d", user->desc().c_str(), user->conv, user->peerPort);
        g_upMgr->push(user->conv, user->peerPort);
    }
};

void onConnect(const httplib::Request& req, httplib::Response& res) {
    std::string strCltPort = req.path_params.at("port");
    LOG_MSG(LogLevel::Info, "onConnect, client addr=%s, client port=%s", req.remote_addr.c_str(), strCltPort.c_str());
    unsigned short nCltPort = atoi(strCltPort.c_str());
    if (nCltPort == 0) {
        LOG_ERR_MSG("onConnect fail, client addr=%s, client port=%s", req.remote_addr.c_str(), strCltPort.c_str());
        res.set_content("param invalid", "text/plain");
        return;
    }

    unsigned int ident = 0;
    unsigned short port = 0;
    if (g_upMgr->pop(ident, port)) {
        char szResp[64] = { 0 };
        snprintf(szResp, sizeof(szResp), "%d:%d", ident, port);
        res.set_content(szResp, "text/plain");
        g_kcpServer->addUser(ident, "127.0.0.1", port, req.remote_addr, nCltPort);
    } else {
        res.set_content("server exception", "text/plain");
    }
}

void onDisconnect(const httplib::Request& req, httplib::Response& res) {
    std::string strCltIdent = req.path_params.at("ident");
    LOG_MSG(LogLevel::Info, "onDisconnect, client addr=%s, client ident=%s", req.remote_addr.c_str(), strCltIdent.c_str());
    unsigned int nCltIdent = atoi(strCltIdent.c_str());
    if (nCltIdent == 0) {
        LOG_ERR_MSG("onDisconnect fail, client addr=%s, client ident=%s", req.remote_addr.c_str(), strCltIdent.c_str());
        res.set_content("param invalid", "text/plain");
        return;
    }
    g_kcpServer->kickUser(nCltIdent);
    res.set_content("OK", "text/plain");
}

void onSignal(int signum) {
    LOG_MSG(LogLevel::Warn, "onSignal: %s", zbf::signal_utils::strsignal(signum));    
    g_kcpServer->stop();
    g_httpServer->stop();
}

void onExit() {
    zbf::logMemTrackStat();
}

int main(int argc, char** argv)
{
    std::atexit(onExit);
    zbf::signal_utils::handleSignal(onSignal, {SIGINT, SIGTERM});

    log_utils::log_level = LogLevel::Trace;
    UserParamMgr upMgr;
    g_upMgr = &upMgr;
    upMgr.init();

    kcpsock_server kcpSvr(new listener, new default_msg_proto());
    g_kcpServer = &kcpSvr;
    kcpSvr.start(2);

    httplib::Server httpSvr;
    g_httpServer = &httpSvr;

    httpSvr.Get("/connect/:port", [](const httplib::Request& req, httplib::Response& res) {
        onConnect(req, res);
    });
    httpSvr.Get("/disconnect/:ident", [](const httplib::Request& req, httplib::Response& res) {
        onDisconnect(req, res);
    });
    LOG_MSG(LogLevel::Info, "http server start: http://0.0.0.0:8082");
    httpSvr.listen("0.0.0.0", 8082);
    
    kcpSvr.serveUtilStop();
    
    return 0;
}
