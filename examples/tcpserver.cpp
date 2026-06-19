
#include "zbf/socket_tcp_v5.hpp"
#include "zbf/signal_utils.hpp"

using namespace zbf;

tcpsock_server* g_server = nullptr;

class user : public tcpsock_user {
public:
    user(): tcpsock_user() {
    }

    virtual void onConnect() {
        LOG_MSG(LogLevel::Info, "onConnect: user=(%s)", this->desc().c_str());
    }
    virtual void onRecvMsg(const tcp_message* msg) {
        auto* proto = dynamic_cast<default_msg_proto*>(getProtocol());
        uint32_t hdrSz = proto ? proto->headerSize() : 0;
        std::string strText(msg->data.data() + hdrSz, msg->data.size() - hdrSz);
        LOG_MSG(LogLevel::Info, "user=(%s) onRecvMsg: %s", this->desc().c_str(), strText.c_str());
        if (0 == strcmp(strText.c_str(), "QUIT")) {
            // g_server->stop();
        } else {
            std::string str = strText + std::string("_reply");
            auto reply = std::unique_ptr<tcp_message>(proto ? proto->genMessage(0x10, str) : nullptr);
            if (reply) post(std::move(reply));
        }
    }
    virtual void onDisconnect() {
        LOG_MSG(LogLevel::Info, "onDisconnect: user=(%s)", this->desc().c_str());
    }
};

class server : public tcpsock_server {
public:
    server(): tcpsock_server(new default_msg_proto) {
    }

    virtual tcpsock_user* createUser() {
        return new user();
    }
};

void onSignal(int signum) {
    LOG_MSG(LogLevel::Warn, "onSignal: %s", zbf::signal_utils::strsignal(signum));
    g_server->stop();
}

void onExit() {
    zbf::logMemTrackStat();
}

int main(int argc, char** argv)
{
    std::atexit(onExit);
    zbf::signal_utils::handleSignal(onSignal, {SIGINT, SIGTERM});
    std::string log_path = log_utils::createLogPath();
    log_utils::open(log_path.c_str(), LogLevel::Trace);
    
    std::vector<HOST_AND_PORT> addrs;
    // addrs.push_back(HOST_AND_PORT("::", 4701));
    // addrs.push_back(HOST_AND_PORT("::1", 4701));
    addrs.push_back(HOST_AND_PORT("0.0.0.0", 4701));

    tcpsock_server* svr = new server();
    g_server = svr;

    svr->open(addrs);
    svr->start(4);
    svr->serveUtilStop();
    svr->close();

    delete svr;
    log_utils::close();
    return 0;
}
