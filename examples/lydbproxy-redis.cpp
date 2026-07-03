
#include "lygc/dbproxy_redis.hpp"
#include "lygc/central_client.hpp"
#include "zbf/signal_utils.hpp"

using zbf::LogLevel;

lygc::DBProxyRedis* g_redisProxy = nullptr;

void onSignal(int signum) {
    LOG_MSG(LogLevel::Warn, "onSignal: %s(%d)", zbf::signal_utils::strsignal(signum), signum);
    if (g_redisProxy) g_redisProxy->stop();
}

void onExit() {
    zbf::logMemTrackStat();
}

int main(int argc, char** argv)
{
    std::atexit(onExit);
    zbf::signal_utils::ignoreSignal(SIGPIPE);
    zbf::signal_utils::handleSignal(onSignal, {SIGINT, SIGTERM});

    // Redis pool config (see test/test_redis.cpp)
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <redis_password>\n", argv[0]);
        return 1;
    }
    wjp::redis_client_connect_param redis_conn_param("127.0.0.1", argv[1], "local");
    redis_conn_param.DBSlotNo = 0;
    lygc::RedisPoolConf redisConf = { .param = redis_conn_param, .pool_size = 2 };

    // Fetch server config from Central
    std::string conf = lygc::Central::get("config");
    lygc::ServerGroupMap cfg;
    cfg.loadFromString(conf);

    lygc::ServerConfig serverConf = cfg.groups["redis-proxy"].at(0);
    serverConf.host = "::";
    lygc::DBProxyRedis proxy(serverConf, zbf::LogLevel::Trace);
    g_redisProxy = &proxy;
    proxy.open();
    proxy.start({redisConf}, 2);

    proxy.serveUtilStop();
    proxy.close();

    return 0;
}
