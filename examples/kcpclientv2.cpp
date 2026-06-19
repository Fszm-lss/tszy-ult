
#include "zbf/socket_kcp_v2.hpp"
#include "fszm/random_utils.hpp"
#include "fszm/curl_helper.hpp"

using namespace zbf;
using namespace fszm;

class listener : public kcpsock_listener {
public:
	void onRecvMsg(std::shared_ptr<kcp_user> user, const tcp_message* msg) {
        std::string body(msg->data.begin() + user->getProtocol()->headerSize(), msg->data.end());
        LOG_MSG(LogLevel::Info, "onRecv, sz=%zu, body=%s", msg->data.size(), body.c_str());
    }
};

void onExit() {
    zbf::logMemTrackStat();
}

int main(int argc, char** argv)
{
    std::atexit(onExit);
    log_utils::log_level = LogLevel::Debug;
    unsigned int ident = 0;    
    std::string localAddr = "0.0.0.0";
    unsigned short localPort = 20080;
    std::string serverAddr = "127.0.0.1";
    unsigned short serverPort = 0;

    char szUrl[128] = {0};
    snprintf(szUrl, sizeof(szUrl), "http://localhost:8082/connect/%d", localPort);

    int httpErr = -1;
    std::string resp = fszm::CurlHelper::doGet(szUrl, 1000, &httpErr);
    if (httpErr != 0 || resp.empty()) {
        LOG_ERR_MSG("http get error: curl code=%d, url=%s", httpErr, szUrl);
        return 1;
    }

    LOG_MSG(LogLevel::Info, "http url=%s, resp=%s", szUrl, resp.c_str());

    ident = atoi(resp.c_str());
    resp = resp.substr(resp.find(':') + 1);
    serverPort = atoi(resp.c_str());

    kcpsock_client client(new listener, new default_msg_proto());
    if (client.open(ident, localAddr, localPort, serverAddr, serverPort)) {
        LOG_ERR_MSG("client open fail");
        return 1;
    }
    client.start();

    for (int i = 0; i < 10; ++i) {
        int length = fszm::random_utils::randomNumber(32, 64);
        std::string randstr = fszm::random_utils::randomString(fszm::Numbers, length);
        // build message with protocol
        default_msg_proto proto;
        auto msg = std::unique_ptr<tcp_message>(proto.genMessage(0x42, randstr));
        if (client.post(std::move(msg))) {
            LOG_ERR_MSG("post fail, queue full");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    client.stop();
    client.close();

    snprintf(szUrl, sizeof(szUrl), "http://localhost:8082/disconnect/%d", ident);
    resp = fszm::CurlHelper::doGet(szUrl, 1000, &httpErr);
    LOG_MSG(LogLevel::Info, "http url=%s, resp=%s", szUrl, resp.c_str());

    return 0;
}
