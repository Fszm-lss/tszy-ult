
#include "zbf/signal_utils.hpp"
#include "zbf/asio_socket_tcp.hpp"

using namespace zbf;

std::shared_ptr<tcpsock_server> g_server;
int g_recvMsgs = 0;

class server : public tcpsock_server {
public:
    using tcpsock_server::tcpsock_server;
    
    virtual void on_connect(std::shared_ptr<tcpsock_user> user) override {
        LOG_MSG(LogLevel::Info, "(%s) on_connect", user->desc().c_str());
        tcpsock_server::on_connect(user);
    }

    virtual void on_message(std::shared_ptr<tcpsock_user> user, std::unique_ptr<tcp_message> msg) override {
        // default_msg_proto: 4-byte header, skip to get body
        std::string strMsg(msg->data.data() + sizeof(uint32_t), msg->data.size() - sizeof(uint32_t));
        LOG_MSG(LogLevel::Info, "(%s) on_message: %s", user->desc().c_str(), strMsg.c_str());
        LOG_MSG(LogLevel::Info, "(%s) total recv: %d", user->desc().c_str(), ++g_recvMsgs);

        std::string str = strMsg + std::string("_reply");
        default_msg_proto proto;
        auto reply = std::unique_ptr<tcp_message>(proto.genMessage(0x10, str));
        user->post(std::move(reply));
    }

    virtual void on_disconnect(std::shared_ptr<tcpsock_user> user) override {
        LOG_MSG(LogLevel::Info, "(%s) on_disconnect", user->desc().c_str());
        tcpsock_server::on_disconnect(user);
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
    if (argc != 3) {
        printf("program <host addr> <port>\n");
        return 1;
    }

    std::atexit(onExit);
    zbf::signal_utils::ignoreSignal(SIGPIPE);
    zbf::signal_utils::handleSignal(onSignal, {SIGINT, SIGTERM});
    
    log_utils::log_level = LogLevel::TraceMore;
    LOG_MSG(LogLevel::Info, "main start");

    unsigned short port = atoi(argv[2]);
    g_server = std::make_shared<server>(std::string(argv[1]), port, new default_msg_proto);
    g_server->start();
    g_server->serveUtilStop();
    g_server.reset();

    LOG_MSG(LogLevel::Info, "main exit");
    return 0;
}
