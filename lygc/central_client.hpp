#pragma once

#include "nlohmann/json.hpp"
#include "fszm/curl_helper.hpp"
#include "zbf/log_utils.hpp"

namespace lygc {

using json = nlohmann::json;
using zbf::LogLevel;

class CentralServClt {
public:
    CentralServClt() {
    }

    ~CentralServClt() {
    }

    static void init(const std::string& addr) {
        if (addr.empty()) {
            LOG_ERR_MSG("CentralServClt init fail, addr is empty");
            return;
        }
        _serverAddr = addr;
        LOG_MSG(LogLevel::Debug, "server addr is set to: %s", _serverAddr.c_str());
    }

    static std::string get(const std::string& api) {
        std::string url = _serverAddr + std::string("/api/") + api;
        std::string result = fszm::CurlHelper::doGet(url.c_str());
        return result;
    }

    static std::string registerApi(const std::string& api, const json& jsonobj) {
        std::string url = _serverAddr + std::string("/register/api");
        json obj;
        obj["api"]  = api;
        obj["data"] = jsonobj;
        std::string postData = obj.dump();
        std::string result = fszm::CurlHelper::doPost(url.c_str(), postData.c_str());
        return result;
    }

    static std::string registerApi(const std::string& api, const std::string& jsonstr) {
        return registerApi(api, json::parse(jsonstr));
    }

private:
    inline static std::string _serverAddr{"http://localhost:8081"};
};

typedef CentralServClt Central;

}
