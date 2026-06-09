
#include "lygc/lymsg_protocol.hpp"
#include "lygc/net_server.hpp"
#include "lygc/central_client.hpp"
#include "zbf/signal_utils.hpp"
#include "lydefine.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;
using zbf::LogLevel;
using lygc::request_id_t;

static lygc::NetServer*    g_localServ = nullptr;
static lygc::NetClient*    g_logicClt = nullptr;

class CommonUserhandler : public lygc::UserHandler {
public:
    request_id_t onRequest(const lygc::lymsg_header* reqHeader, const std::string& reqData, std::string& syncRespData) {
        LOG_MSG(LogLevel::Info, "onRequest: req=%s", lygc::strBrief(reqData).c_str());
        lygc::lymsg_header header;
        memcpy(&header, reqHeader, sizeof(header));

        request_id_t asyncReqId = g_localServ->genRequestId();
        header.serial = asyncReqId;
        g_logicClt->request(&header, reqData, [&](const lygc::lymsg_header* respHeader, const std::string& respData) {
            LOG_MSG(LogLevel::Info, "onLambda: resp=%s", lygc::strBrief(respData).c_str());
            g_localServ->response(respHeader->serial, respData);
        });
        return asyncReqId;
    }
};

void onSignal(int signum) {
    LOG_MSG(LogLevel::Warn, "onSignal: %s(%d)", zbf::signal_utils::strsignal(signum), signum);
    g_localServ->stop();
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

    lygc::ServerConfig commonConf = cfg.groups["common"].at(0);
    lygc::ServerConfGroup logicGroup = cfg.groups["logic"];
    
    commonConf.host = "::";
    lygc::NetServer server(commonConf, zbf::LogLevel::Debug);
    g_localServ = &server;
    server.registerUserHandler(LYCOMMON_CLIENT_ORGIN, new CommonUserhandler);
    g_logicClt = server.addAsynClient(logicGroup);

    server.open();
    server.start(4, 1);
    
    server.serveUtilStop();
    server.close();

    return 0;
}
