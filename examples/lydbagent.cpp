
#include "lygc/lymsg_protocol.hpp"
#include "lygc/net_server.hpp"
#include "lygc/dbclient_mysql.hpp"
#include "lygc/central_client.hpp"
#include "zbf/signal_utils.hpp"
#include "lydefine.h"

using zbf::LogLevel;
using lygc::request_id_t;

static lygc::NetServer*       g_localServ = nullptr;
static lygc::DBClientMySQL*   g_mysqlClient = nullptr;


class AgentUserhandler : public lygc::UserHandler {
public:
    request_id_t onRequest(const lygc::lymsg_header* reqHeader, const std::string& reqData, std::string& syncRespData) {
        LOG_MSG(LogLevel::Info, "onRequest: req=%s", lygc::strBrief(reqData).c_str());
        lygc::lymsg_header header;
        memcpy(&header, reqHeader, sizeof(header));

        request_id_t asyncReqId = g_localServ->genRequestId();
        header.serial = asyncReqId;

        std::string sql_query = "select * from testdb.student";
        auto req = wjp::mysql_client_req::newQueryForResult(0, sql_query.c_str());
        g_mysqlClient->request(&header, req.get(), [this](const lygc::lymsg_header* respHeader, const wjp::mysql_client_resp* resp) {
            showResp(resp);
            std::string respData = "OK";
            g_localServ->response(respHeader->serial, respData);
        });

        return asyncReqId;
    }

    void showResp(const wjp::mysql_client_resp* resp) {
        printf("showResp:\n");
        printf("\treq_id=%ld\n", resp->req_id);
        printf("\trc=%d\n", resp->rc);
        printf("\tresults:\n");
        for (int i = 0; i < resp->results.size(); ++i) {
            printf("\t\ttable_%d:\n", i+1);
            fszm::DataTable tab = resp->results[i];
            for (auto row : tab) {
                for (auto str : row) {
                    printf("\t\t%s", str.c_str());
                }
                printf("\n");
            }
        }
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

    lygc::ServerConfig agentConf = cfg.groups["dbagent"].at(0);
    lygc::ServerConfGroup mysqlProxyGrp = cfg.groups["mysql-proxy"];

    agentConf.host = "::";
    lygc::NetServer server(agentConf, zbf::LogLevel::Trace);
    g_localServ = &server;
    server.registerUserHandler(LYDBAGENT_CLIENT_ORGIN, new AgentUserhandler);
    lygc::NetClient* dbproxyClt = server.addAsynClient(mysqlProxyGrp);
    g_mysqlClient = new lygc::DBClientMySQL(dbproxyClt);

    server.open();
    server.start(4, 1);
    
    server.serveUtilStop();
    delete g_mysqlClient;
    server.close();

    return 0;
}
