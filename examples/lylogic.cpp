
#include "lygc/lymsg_protocol.hpp"
#include "lygc/net_server.hpp"
#include "lygc/central_client.hpp"
#include "zbf/signal_utils.hpp"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

using zbf::LogLevel;
using lygc::request_id_t;

static lygc::NetServer* g_localServer = nullptr;

class lycommon_handler : public lygc::UserHandler {
public:
    request_id_t onRequest(const lygc::lymsg_header* reqHeader, const std::string& reqData, std::string& respData) {
        json jobj = json::parse(reqData);
        jobj["resp"] = "success";
        respData = jobj.dump();

        LOG_MSG(LogLevel::Info, "onRequest: req=%s, resp=%s", lygc::strBrief(reqData).c_str(), lygc::strBrief(respData).c_str());
        return lygc::RespType::SYNC_RESPONSE;
    }
};

class lygate_handler : public lygc::UserHandler {
public:
    request_id_t onRequest(const lygc::lymsg_header* reqHeader, const std::string& reqData, std::string& respData) {
        json jobj = json::parse(reqData);
        jobj["resp"] = "success";
        respData = jobj.dump();

        LOG_MSG(LogLevel::Info, "onRequest: req=%s, resp=%s", lygc::strBrief(reqData).c_str(), lygc::strBrief(respData).c_str());
        return lygc::RespType::SYNC_RESPONSE;
    }
};

void onSignal(int signum) {
    LOG_MSG(LogLevel::Warn, "onSignal: %s(%d)", zbf::signal_utils::strsignal(signum), signum);
    g_localServer->stop();
}

void onExit() {
    zbf::logMemTrackStat();
}

int main(int argc, char** argv)
{
    std::atexit(onExit);
    zbf::signal_utils::ignoreSignal(SIGPIPE);
    zbf::signal_utils::handleSignal(onSignal, {SIGINT, SIGTERM});

    std::string config = lygc::Central::get("config");
    lygc::ServerGroupMap cfg;
    cfg.loadFromString(config);

    lygc::ServerConfig gateConf   = cfg.groups["gate"].at(0);
    lygc::ServerConfig commonConf = cfg.groups["common"].at(0);
    lygc::ServerConfig logicConf  = cfg.groups["logic"].at(0);

    logicConf.host = "::";
    lygc::NetServer server(logicConf, zbf::LogLevel::Debug);
    g_localServer = &server;
    server.registerUserHandler(commonConf.origin, new lycommon_handler);
    server.registerUserHandler(gateConf.origin,   new lygate_handler);

    server.open();
    server.start(2);

    server.serveUtilStop();
    server.close();
    
    return 0;
}
