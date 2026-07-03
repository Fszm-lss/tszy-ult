
#include "lygc/dbproxy_mongo.hpp"
#include "lygc/central_client.hpp"
#include "zbf/signal_utils.hpp"

using zbf::LogLevel;

lygc::DBProxyMongo* g_mongoProxy = nullptr;

void onSignal(int signum) {
    LOG_MSG(LogLevel::Warn, "onSignal: %s(%d)", zbf::signal_utils::strsignal(signum), signum);
    if (g_mongoProxy) g_mongoProxy->stop();
}

void onExit() {
    zbf::logMemTrackStat();
}

int main(int argc, char** argv)
{
    std::atexit(onExit);
    zbf::signal_utils::ignoreSignal(SIGPIPE);
    zbf::signal_utils::handleSignal(onSignal, {SIGINT, SIGTERM});

    // MongoDB pool config (see test/test_mongo.cpp)
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <mongo_user> <mongo_password>\n", argv[0]);
        return 1;
    }
    char mongo_uri[256];
    snprintf(mongo_uri, sizeof(mongo_uri), "mongodb://%s:%s@127.0.0.1/?tls=false", argv[1], argv[2]);
    wjp::mongo_client_connect_param mongo_conn_param = {
        .conn_uri = mongo_uri,
        .dbname   = "test1",
        .DBSlotNo = 0
    };
    lygc::MongoPoolConf mongoConf = { .param = mongo_conn_param, .pool_size = 1 };

    // Fetch server config from Central
    std::string conf = lygc::Central::get("config");
    lygc::ServerGroupMap cfg;
    cfg.loadFromString(conf);

    lygc::ServerConfig serverConf = cfg.groups["mongo-proxy"].at(0);
    serverConf.host = "::";
    lygc::DBProxyMongo proxy(serverConf, zbf::LogLevel::Trace);
    g_mongoProxy = &proxy;
    proxy.open();
    proxy.start({mongoConf}, 1);

    proxy.serveUtilStop();
    proxy.close();

    return 0;
}
