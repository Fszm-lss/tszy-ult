
#include "fszm/random_utils.hpp"
#include "lygc/lymsg_protocol.hpp"
#include "lydefine.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;
using namespace zbf;
using namespace lygc;

static async_client_manager* g_asyncMgr = nullptr;

class common_client : public tcpsock_ha_asynclt {
public:
	void onResponse(const tcp_message* msg) {
        lymsg_header respHeader;
        std::string respData;
        lymsg_helper::unpackMsg(msg, respHeader, respData);
        LOG_MSG(LogLevel::Info, "onResponse: msg=%s, data=%s", LYMSG_DESC(&respHeader).c_str(), lygc::strBrief(respData).c_str());
    }
};

void worker(void* param) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    common_client* client = (common_client*) param;
    lymsg_header header;
    // char szMsg[64] = {0};
    int length = fszm::random_utils::randomNumber(512, 16384); // 0.5K ~ 16k
    std::string randstr = fszm::random_utils::randomString(fszm::NumbersAndLetters, length);
    for (int i = 1; i <= 10; ++i) {
        // snprintf(szMsg, sizeof(szMsg), "Yekajielinna_%d", i);

        header.origin = LYCOMMON_CLIENT_ORGIN;
        header.type = 0x20;
        header.serial = i;
        header.param = 0;

        json jobj;
        jobj["msg"] = randstr;
        std::string jsonstr = jobj.dump();
        header.size = jsonstr.size();
        tcp_message* msg = lymsg_helper::packMsg(&header, jsonstr);
        int rc = client->post(msg);
        if (rc) {
            LOG_ERR_MSG("post fail: msg=%s, rc=%d", LYMSG_DESC(&header).c_str(), rc);
            delete msg;
        } else {
            LOG_MSG(LogLevel::Info, "post success: msg=%s, rc=%d", LYMSG_DESC(&header).c_str(), rc);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    g_asyncMgr->stop();
}

void onExit() {
    zbf::logMemTrackStat();
}

int main(int argc, char** argv)
{
    std::atexit(onExit);
    std::string log_path = log_utils::getModulePath();
    log_path += ".log";
    log_utils::open(log_path.c_str(), LogLevel::Debug);

    lymsg_protocol* proto = new lymsg_protocol;
    async_client_manager cltMgr(proto);
    g_asyncMgr = &cltMgr;    
    common_client* client = new common_client();
    client->addConnect("127.0.0.1", 35102, proto);
    client->addConnect("127.0.0.1", 35112, proto);
    client->addConnect("::1", 35102, proto);
    cltMgr.manageClient(client);
    cltMgr.start(1);

    std::thread* thd = new std::thread([&](){
        worker(client);
    });

    cltMgr.serveUtilStop();
    if (thd->joinable()) {
        thd->join();
    }
    delete thd;
    
    log_utils::close();
    return 0;
}
