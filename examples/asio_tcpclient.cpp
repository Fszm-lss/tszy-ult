
#include "zbf/asio_socket_tcp.hpp"
#include <future>

using namespace zbf;

int g_recvMsgs = 0;

class listener : public tcpsock_listener {
public:
    virtual void on_connect(std::shared_ptr<tcpsock_user> user) override {
        LOG_MSG(LogLevel::Info, "(%s) on_connect", user->desc().c_str());
    }

    virtual void on_message(std::shared_ptr<tcpsock_user> user, std::unique_ptr<tcp_message> msg) override {
        std::string strMsg(msg->data.data() + sizeof(uint32_t), msg->data.size() - sizeof(uint32_t));
        LOG_MSG(LogLevel::Info, "(%s) on_message: %s", user->desc().c_str(), strMsg.c_str());
        LOG_MSG(LogLevel::Info, "(%s) total recv: %d", user->desc().c_str(), ++g_recvMsgs);
    }

    virtual void on_disconnect(std::shared_ptr<tcpsock_user> user) override {
        LOG_MSG(LogLevel::Info, "(%s) on_disconnect", user->desc().c_str());
    }
};

void onExit() {
    zbf::logMemTrackStat();
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        printf("program <server addr> <server port> [async]\n");
        return 1;
    }

    std::atexit(onExit);
    log_utils::log_level = LogLevel::TraceMore;
    unsigned short port = atoi(argv[2]);
    bool async = (argc >= 4 && std::string(argv[3]) == "async");

    default_msg_proto* proto = new default_msg_proto;
    auto clt = std::make_unique<tcpsock_client>(argv[1], port, proto, std::make_unique<listener>());
    clt->start();

    if (async) {
        LOG_MSG(LogLevel::Info, "=== async_connect mode ===");
        std::promise<int> promise;
        auto future = promise.get_future();
        clt->async_connect([&promise](int rc) { promise.set_value(rc); }, 1);
        int rc = future.get();
        if (rc != 0) {
            LOG_MSG(LogLevel::Error, "async_connect fail, rc=%d", rc);
            clt->stop();
            return 1;
        }
        LOG_MSG(LogLevel::Info, "async_connect success");
    } else {
        LOG_MSG(LogLevel::Info, "=== sync connect mode ===");
        clt->connect();
    }

    char szBuf[32] = {0};
    for (int i = 0; i < 10; ++i) {
        snprintf(szBuf, sizeof(szBuf), "message_%d", i+1);
        std::string strMsg(szBuf);
        auto msg = std::unique_ptr<tcp_message>(proto->genMessage(0x10, strMsg));
        clt->post(std::move(msg));
    }
    
    std::this_thread::sleep_for(std::chrono::seconds(7));
    clt->disconnect();
    clt->stop();
    return 0;
}
