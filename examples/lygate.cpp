
#include "lygc/lymsg_protocol.hpp"
#include "lygc/gate_server.hpp"
#include "lygc/central_client.hpp"
#include "zbf/signal_utils.hpp"
#include "lydefine.h"

using zbf::LogLevel;
using lygc::request_id_t;

static lygc::GateServer*   g_gateServer = nullptr;
static lygc::NetClient*    g_logicClt = nullptr;

class GateUserHandler : public lygc::UserHandler {
public:
    request_id_t onRequest(const lygc::lymsg_header* reqHeader, const std::string& reqData, std::string& syncRespData) override {
        LOG_MSG(LogLevel::Info, "onRequest: req=%s", lygc::strBrief(reqData).c_str());
        lygc::lymsg_header header;
        memcpy(&header, reqHeader, sizeof(header));

        request_id_t asyncReqId = g_gateServer->genRequestId();
        header.serial = asyncReqId;
        g_logicClt->request(&header, reqData, [](const lygc::lymsg_header* respHeader, const std::string& respData) {
            LOG_MSG(LogLevel::Info, "onLambda: resp=%s", lygc::strBrief(respData).c_str());
            g_gateServer->response(respHeader->serial, respData);
        });
        return asyncReqId;
    }

};

void onSignal(int signum) {
    LOG_MSG(LogLevel::Warn, "onSignal: %s(%d)", zbf::signal_utils::strsignal(signum), signum);
    g_gateServer->stop();
}

void onExit() {
    zbf::logMemTrackStat();
}

int main(int argc, char** argv)
{
    std::atexit(onExit);
    zbf::signal_utils::ignoreSignal(SIGPIPE);
    zbf::signal_utils::handleSignal(onSignal, {SIGINT, SIGTERM});

    std::string conf = lygc::Central::get("config");
    lygc::ServerGroupMap cfg;
    cfg.loadFromString(conf);

    lygc::ServerConfGroup logicGroup = cfg.groups["logic"];
    lygc::ServerConfig gateConf = cfg.groups["gate"].at(0);

    gateConf.host = "::";
    lygc::GateServer server(gateConf, zbf::LogLevel::Trace);
    g_gateServer = &server;
    server.loadPrivateKey("key/rsa_private_key.pem");

    // handle gate client
    server.registerUserHandler(LYGATE_CLIENT_ORGIN, new GateUserHandler);
    // access to logic server
    g_logicClt = server.addAsynClient(logicGroup);

    server.open();
    server.start(4, 1);
    
    server.serveUtilStop();
    server.close();

    return 0;
}
