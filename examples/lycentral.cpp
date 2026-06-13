
#include <fstream>
#include <iterator>
#include "zbf/log_utils.hpp"
#include "zbf/signal_utils.hpp"
#include "lygc/central_server.hpp"

using namespace zbf;
using namespace lygc;

CentralServer* g_cs = nullptr;

void onSignal(int signum) {
    LOG_MSG(LogLevel::Warn, "onSignal: %s(%d)", zbf::signal_utils::strsignal(signum), signum);
    g_cs->stop();
}

int main(int argc, char** argv)
{
    signal_utils::ignoreSignal(SIGPIPE);
    signal_utils::handleSignal(onSignal, {SIGINT, SIGTERM});
    std::string log_path = log_utils::createLogPath();
    log_utils::open(log_path.c_str(), LogLevel::Trace);
    
    std::string data;
    std::ifstream ifs("lycentral-conf.json");
    if (ifs) {
        data.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }
    lygc::CentralServer cs;
    g_cs = &cs;
    cs.set("config", data);
    cs.registerApi();
    cs.start();

    LOG_MSG(LogLevel::Debug, "CentralServer exit");
    return 0;
}
