#pragma once

#include <unordered_map>
#include <fstream>
#include "nlohmann/json.hpp"
#include "zbf/log_utils.hpp"

namespace lygc {

using json = nlohmann::json;
using zbf::LogLevel;

struct ServerConfig {
    ServerConfig() : origin(0), port(0) {
    }

    ServerConfig(const std::string& name, unsigned short origin, const std::string& host, unsigned short port) {
        this->name  = name;
        this->origin = origin;
        this->host  = host;
        this->port  = port;
    }

    explicit ServerConfig(const std::string& jsonstr) {
        try {
            json obj = json::parse(jsonstr);
            name  = obj["name"].get<std::string>();
            origin = (unsigned short) obj["origin"].get<int>();
            host  = obj["host"].get<std::string>();
            port  = (unsigned short) obj["port"].get<int>();
        } catch(const json::exception& e) {
            LOG_ERR_MSG("ServerConfig() fail, err=%s, jsonstr=%s", e.what(), jsonstr.c_str());
        }
    }

    ServerConfig(const ServerConfig& copy) {
        this->name  = copy.name;
        this->origin = copy.origin;
        this->host  = copy.host;
        this->port  = copy.port;
    }

    json toJson() {
        json obj;
        obj["name"]  = name;
        obj["origin"] = origin;
        obj["host"]  = host;
        obj["port"]  = port;
        return obj;
    }

    std::string toJsonStr() {
        return toJson().dump();
    }

    std::string    name;
    unsigned short origin;
    std::string    host;
    unsigned short port;
};

typedef std::vector<ServerConfig> ServerConfGroup;

struct ServerGroupMap {
    
    std::unordered_map<std::string, ServerConfGroup> groups;

    int loadFromFile(const std::string& path) {
        int rc = 0;
        try {
            std::ifstream file(path);
            if (file.is_open()) {
                json jRoot = json::parse(file);
                load(jRoot);
            } else {
                LOG_ERR_MSG("loadFromFile fail, file not found: %s", path.c_str());
                rc = -2;
            }
        } catch(const json::exception& e) {
            LOG_ERR_MSG("loadFromFile fail, err=%s, file=%s", e.what(), path.c_str());
            rc = -1;
        }
        return rc;
    }

    int loadFromString(const std::string& str) {
        int rc = 0;
        try {
            json jRoot = json::parse(str);
            load(jRoot);
        } catch(const json::exception& e) {
            LOG_ERR_MSG("loadFromString fail, err=%s, str=%s", e.what(), str.c_str());
            rc = -1;
        }
        return rc;
    }

private:
    int load(const json& jRoot) {
        groups.clear();
        for (auto& [key, value] : jRoot.items()) {
            std::string servType = key;
            json jGroup = value;
            for (int i = 0; i < jGroup.size(); ++i) {
                json jServ = jGroup[i];
                ServerConfig servConf;
                servConf.name  = jServ["name"].get<std::string>();
                servConf.origin = (unsigned short) jServ["origin"].get<int>();
                servConf.host  = jServ["host"].get<std::string>();
                servConf.port  = (unsigned short) jServ["port"].get<int>();
                groups[servType].push_back(servConf);
            }
        }
        return 0;
    }
};


}
