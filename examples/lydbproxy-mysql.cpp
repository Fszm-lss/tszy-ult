
#include "lygc/dbproxy_mysql.hpp"
#include "lygc/central_client.hpp"
#include "zbf/signal_utils.hpp"

using zbf::LogLevel;

lygc::DBProxyMySQL* g_mysqlProxy = nullptr;

void onSignal(int signum) {
    LOG_MSG(LogLevel::Warn, "onSignal: %s(%d)", zbf::signal_utils::strsignal(signum), signum);
    if (g_mysqlProxy) g_mysqlProxy->stop();
}

void onExit() {
    zbf::logMemTrackStat();
}

int main(int argc, char** argv)
{
    std::atexit(onExit);
    zbf::signal_utils::ignoreSignal(SIGPIPE);
    zbf::signal_utils::handleSignal(onSignal, {SIGINT, SIGTERM});

    // MySQL pool config
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <mysql_user> <mysql_password>\n", argv[0]);
        return 1;
    }
    wjp::mysql_client_connect_param mysql_conn_param("127.0.0.1", argv[1], argv[2], "testdb");
    mysql_conn_param.DBSlotNo = 0;
    lygc::MySQLPoolConf mysqlConf = { .param = mysql_conn_param, .pool_size = 2 };

    // Fetch server config from Central
    std::string conf = lygc::Central::get("config");
    lygc::ServerGroupMap cfg;
    cfg.loadFromString(conf);

    lygc::ServerConfig serverConf = cfg.groups["mysql-proxy"].at(0);
    serverConf.host = "::";
    lygc::DBProxyMySQL proxy(serverConf, zbf::LogLevel::Trace);
    g_mysqlProxy = &proxy;
    proxy.open();
    proxy.start({mysqlConf}, 2);

    proxy.serveUtilStop();
    proxy.close();

    return 0;
}
