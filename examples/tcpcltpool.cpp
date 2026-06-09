
#include "zbf/socket_tcp_v5.hpp"

using namespace zbf;

tcpsock_cltpool* g_pool = nullptr;
default_msg_proto* g_proto = nullptr;

void worker(int seq, int times) {
    log_utils::registerThreadId((short) seq);

    char szMsg[64] = {0};
    for (int i = 1; i <= times; ++i) {
        snprintf(szMsg, sizeof(szMsg), "client_%d_msg_%d", seq, i);
        tcp_message* msg = g_proto->genMessage(0x10, std::string(szMsg));
        tcp_message* reply = nullptr;
        int rc = g_pool->send(msg, &reply, 1000);
        if (!rc && reply) {
            uint32_t hdrSz = g_proto->headerSize();
            std::string strMsg(reply->data.data() + hdrSz, reply->data.size() - hdrSz);
            LOG_MSG(LogLevel::Info, "client(%d) got reply:%s", seq, strMsg.c_str());
            delete reply;
        }
        delete msg;
        int w = rand() % 10 + 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(w));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void onExit() {
    zbf::logMemTrackStat();
}

int main(int argc, char** argv)
{
    std::atexit(onExit);
    std::string log_path = log_utils::createLogPath();
    log_utils::open(log_path.c_str(), LogLevel::Trace);

    default_msg_proto* proto = new default_msg_proto;
    g_proto = proto;
    tcpsock_cltpool pool(proto);
    pool.open("127.0.0.1", 4701, 1);
    LOG_MSG(LogLevel::Info, "connect to server success, pool=(%s)", pool.desc().c_str());
    g_pool = &pool;

    int count = 1;
    std::vector<std::thread*> thds;
    for (int i = 0; i < count; ++i) {
        std::thread* thd = new std::thread([&, i](){
            worker(i+1, 10);
        });
        thds.push_back(thd);
    }

    for (int i = 0; i < count; ++i) {
        std::thread* thd = thds[i];
        thd->join();
        delete thd;
    }
    thds.clear();

    g_pool = nullptr;
    pool.close();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // tell server quit
    std::string quit = "QUIT";
    tcpsock_client clt("127.0.0.1", 4701, proto);
    clt.connect();
    tcp_message* msg = proto->genMessage(0x10, quit);
    clt.send(msg);
    delete msg;
    
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    clt.disconnect();
    
    log_utils::close();
    return 0;
}
