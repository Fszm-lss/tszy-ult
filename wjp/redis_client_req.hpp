#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "zbf/mem_alloc.hpp"
#include "fszm/serialize_helper.hpp"

namespace wjp {

using StringArray = fszm::DataRow;
using fszm::StringMap;
using fszm::ScoreArray;
using fszm::serialize_helper;

enum redis_client_req_type {
    execCommandStat,  // input: argv, result: bool
    execCommandInt,   // input: argv, result: int
    execCommandStr,   // input: argv, result: string
    execCommandSA,    // input: argv, result: StringArray
    execCommandSM,    // input: argv, result: StringMap
    execCommandSCA,   // input: argv, result: ScoreArray
    redisReqNone
};

typedef uint64_t redis_request_id_t;

struct redis_client_req : zbf::object_tracker<redis_client_req> {
    uint16_t                     version{0};
    redis_client_req_type        type{redisReqNone};
    uint8_t                      db_slot{0};
    std::vector<std::string>     argv;
    // params end
    redis_request_id_t           req_id{0};
    int                          retry_times{0};

    std::string serialize() const {
        serialize_helper helper;
        helper.serialize_uint16(version);
        helper.serialize_int8(type);
        helper.serialize_uint8(db_slot);
        helper.serialize_int32(argv.size());
        for (const auto& a : argv)
            helper.serialize_str(a);
        return helper.data();
    }

    static std::unique_ptr<redis_client_req> deserialize(const std::string& data) {
        serialize_helper helper(data);
        auto req = std::unique_ptr<redis_client_req>(new redis_client_req);
        req->version = helper.deserialize_uint16();
        req->type = (redis_client_req_type) helper.deserialize_int8();
        req->db_slot = helper.deserialize_uint8();
        int32_t argc = helper.deserialize_int32();
        for (int i = 0; i < argc; ++i)
            req->argv.push_back(helper.deserialize_str());
        return req;
    }

    std::string desc() const {
        std::string cmdStr;
        for (size_t i = 0; i < argv.size(); ++i) {
            if (i > 0) cmdStr += " ";
            cmdStr += argv[i];
        }
        std::string str = std::string("{ver=")  + std::to_string(version);
        str += std::string(",db=")   + std::to_string(db_slot);
        str += std::string(",type=") + std::to_string((int)type);
        str += std::string(",argv=") + cmdStr;
        str += std::string(",req=")  + std::to_string(req_id) + std::string("}");
        return str;
    }
};

struct redis_client_resp : zbf::object_tracker<redis_client_resp> {
    uint16_t              version{0};
    redis_request_id_t    req_id{0};
    redis_client_req_type type{redisReqNone};
    int                   rc{-1};
    bool                  resStat{false};
    int64_t               resInt{0};
    std::string           resStr;
    StringArray           resSA;
    StringMap             resSM;
    ScoreArray            resSCA;

    std::string serialize() const {
        serialize_helper helper;
        helper.serialize_uint16(version);
        helper.serialize_int64(req_id);
        helper.serialize_int8(type);
        helper.serialize_int32(rc);

        if (type == execCommandStat) {
            helper.serialize_int8(resStat ? 1 : 0);
        } else if (type == execCommandInt) {
            helper.serialize_int64(resInt);
        } else if (type == execCommandStr) {
            helper.serialize_str(resStr);
        } else if (type == execCommandSA) {
            helper.serialize_row(resSA);
        } else if (type == execCommandSM) {
            helper.serialize_string_map(resSM);
        } else if (type == execCommandSCA) {
            helper.serialize_score_array(resSCA);
        }
        return helper.data();
    }

    static std::unique_ptr<redis_client_resp> deserialize(const std::string& data) {
        serialize_helper helper(data);
        auto resp = std::unique_ptr<redis_client_resp>(new redis_client_resp);
        resp->version = helper.deserialize_uint16();
        resp->req_id = helper.deserialize_int64();
        resp->type   = (redis_client_req_type) helper.deserialize_int8();
        resp->rc     = helper.deserialize_int32();

        if (resp->type == execCommandStat) {
            resp->resStat = helper.deserialize_int8() != 0;
        } else if (resp->type == execCommandInt) {
            resp->resInt = helper.deserialize_int64();
        } else if (resp->type == execCommandStr) {
            resp->resStr= helper.deserialize_str();
        } else if (resp->type == execCommandSA) {
            resp->resSA = helper.deserialize_row();
        } else if (resp->type == execCommandSM) {
            resp->resSM = helper.deserialize_string_map();
        } else if (resp->type == execCommandSCA) {
            resp->resSCA = helper.deserialize_score_array();
        }
        return resp;
    }
};


}
