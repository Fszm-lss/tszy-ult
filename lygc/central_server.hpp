#pragma once

#include <map>
#include "httplib.h"
#include "nlohmann/json.hpp"
#include "zbf/log_utils.hpp"

namespace lygc {

using json = nlohmann::json;
using zbf::LogLevel;

class CentralServer {
public:
    CentralServer() {
    }

    ~CentralServer() {
    }

    void start(unsigned short port = 8081) {
        LOG_MSG(LogLevel::Debug, "CentralServer start, port=%d", port);
        _httpServer.listen("0.0.0.0", port);
    }

    void stop() {
        _httpServer.stop();
        LOG_MSG(LogLevel::Debug, "CentralServer stop");
    }

    void set(const std::string& api, const std::string& data) {
        LOG_MSG(LogLevel::Trace, "set, api=%s, data=%s", api.c_str(), data.c_str());
        _api2data[api] = data;

        _httpServer.Get("/api/:api", [this](const httplib::Request& req, httplib::Response& res) {
            onApi(req, res);
        });
    }

    // curl -X POST -H "Content-Type: application/json" -d '{"host":"localhost"}' http://localhost:8081/register/api
    void registerApi() {
        _httpServer.Post("/register/api", [this](const httplib::Request& req, httplib::Response& res) {
            onRegisterApi(req, res);
        });
    }

private:
    void onRegisterApi(const httplib::Request& req, httplib::Response& res) {
        std::string jsonstr = req.body;
        LOG_MSG(LogLevel::Trace, "onRegisterApi, client=%s, jsonstr=%s", req.remote_addr.c_str(), jsonstr.c_str());
        std::string api;
        std::string data;
        if (0 == extractApiAndData(jsonstr, api, data)) {
            set(api, data);
            res.set_content("OK", "text/plain");
        } else {
            res.set_content("Fail", "text/plain");
        }
    }

    void onApi(const httplib::Request& req, httplib::Response& res) {
        std::string api = req.path_params.at("api");
        auto it = _api2data.find(api);
        if (it != _api2data.end()) {
            std::string data = it->second;
            res.set_content(data, "text/plain");
            LOG_MSG(LogLevel::Trace, "onApi, client=%s, api=%s, data=%s", req.remote_addr.c_str(), api.c_str(), data.c_str());
        } else {
            res.set_content("None", "text/plain");
            LOG_ERR_MSG("onApi fail(api not exist), client=%s, api=%s", req.remote_addr.c_str(), api.c_str());
        }
    }

    int extractApiAndData(const std::string& jsonstr, std::string& api, std::string& data) {
        int rc = -1;
        try {
            json obj = json::parse(jsonstr);
            api = obj["api"].get<std::string>();
            data = obj["data"].get<json>().dump();
            rc = 0;
        } catch(const json::exception& e) {
            LOG_ERR_MSG("extractApiAndData() fail, err=%s, jsonStr=%s", e.what(), jsonstr.c_str());
        }
        return rc;
    }

private:
    httplib::Server _httpServer;
    std::map<std::string, std::string> _api2data;
};


}
