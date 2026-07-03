#pragma once
// 2025-12

#include <memory>
#include "zbf/mem_alloc.hpp"
#include "fszm/serialize_helper.hpp"

namespace wjp {

using fszm::DataRow;
using fszm::DataTable;
using fszm::DataTables;
using fszm::FieldsType;
using fszm::serialize_helper;

typedef uint64_t mysql_request_id_t;

enum mysql_client_req_type {
    execQuery,           // execQuery(sql, transaction)
    execRealQuery,       // execQuery(sql, length, transaction)
    execQueryForResult,  // execQuery(sql, table)
    execStatement,       // execStatement(sql, params, paramCols, resultCols, results, transaction)
    mysqlReqNone
};

struct mysql_stmt_req_params : zbf::object_tracker<mysql_stmt_req_params> {
    DataTable             params;
    int                   paramCols;
    int                   resultCols;

    mysql_stmt_req_params() : paramCols(0), resultCols(0) {
    }

    mysql_stmt_req_params(const DataTable& params, const int paramCols, const int resultCols)
        : params(params), paramCols(paramCols), resultCols(resultCols) {
    }
};

struct mysql_client_req : zbf::object_tracker<mysql_client_req> {
    // params start
    uint16_t               version{0};
    mysql_client_req_type  type;
    uint8_t                db_slot{0};
    std::string            sql;
    bool                   transaction;
    mysql_stmt_req_params* stmt_param;
    // params end
    mysql_request_id_t     req_id;
    int                    retry_times;

    mysql_client_req(mysql_client_req_type type, uint8_t db_slot, const std::string& sql, bool transaction = false)
        : type(type), db_slot(db_slot), sql(sql), transaction(transaction), stmt_param(nullptr), req_id(0), retry_times(0) {
    }

    mysql_client_req(const mysql_client_req&) = delete;
    mysql_client_req& operator=(const mysql_client_req&) = delete;

    virtual ~mysql_client_req() {
        if (stmt_param) delete stmt_param;
    }

    static std::unique_ptr<mysql_client_req> newQuery(uint8_t db_slot, const char* sql, bool transaction = false) {
        if (!sql) return nullptr;
        return std::unique_ptr<mysql_client_req>(new mysql_client_req(mysql_client_req_type::execQuery, db_slot, std::string(sql), transaction));
    }

    static std::unique_ptr<mysql_client_req> newQuery(uint8_t db_slot, const char* sql, unsigned long length, bool transaction = false) {
        if (!sql) return nullptr;
        return std::unique_ptr<mysql_client_req>(new mysql_client_req(mysql_client_req_type::execRealQuery, db_slot, std::string(sql, length), transaction));
    }

    static std::unique_ptr<mysql_client_req> newQueryForResult(uint8_t db_slot, const char* sql) {
        if (!sql) return nullptr;
        return std::unique_ptr<mysql_client_req>(new mysql_client_req(mysql_client_req_type::execQueryForResult, db_slot, std::string(sql)));
    }

    static std::unique_ptr<mysql_client_req> newStatement(uint8_t db_slot, const char* sql, const DataTable& params, int paramCols, int resultCols, bool transaction = false) {
        if (!sql) return nullptr;
        auto req = std::unique_ptr<mysql_client_req>(new mysql_client_req(mysql_client_req_type::execStatement, db_slot, std::string(sql), transaction));
        req->stmt_param = new mysql_stmt_req_params(params, paramCols, resultCols);
        return req;
    }

    std::string serialize() const {
        serialize_helper helper;
        helper.serialize_uint16(version);
        helper.serialize_int8((int8_t)type);
        helper.serialize_uint8(db_slot);
        helper.serialize_str(sql);
        helper.serialize_bool(transaction);
        bool param_exist = (stmt_param != nullptr);
        helper.serialize_bool(param_exist);
        if (param_exist) {
            helper.serialize_table(stmt_param->params);
            helper.serialize_int32(stmt_param->paramCols);
            helper.serialize_int32(stmt_param->resultCols);
        }
        return helper.data();
    }

    std::string desc() const {
        std::string str = std::string("{ver=")  + std::to_string(version);
        str += std::string(",db=")   + std::to_string(db_slot);
        str += std::string(",type=") + std::to_string((int)type);
        str += std::string(",sql=")  + sql;
        str += std::string(",req=")  + std::to_string(req_id) + std::string("}");
        return str;
    }

    static std::unique_ptr<mysql_client_req> deserialize(const std::string& data) {
        serialize_helper helper(data);
        mysql_client_req* req = new mysql_client_req(mysql_client_req_type::mysqlReqNone, 0, std::string());
        req->version = helper.deserialize_uint16();
        req->type = (mysql_client_req_type) helper.deserialize_int8();
        req->db_slot = helper.deserialize_uint8();
        req->sql = helper.deserialize_str();
        req->transaction = helper.deserialize_bool();
        bool param_exist = helper.deserialize_bool();
        if (param_exist) {
            req->stmt_param = new mysql_stmt_req_params;
            req->stmt_param->params = helper.deserialize_table();
            req->stmt_param->paramCols = helper.deserialize_int32();
            req->stmt_param->resultCols = helper.deserialize_int32();
        }
        return std::unique_ptr<mysql_client_req>(req);
    }
};

struct mysql_client_resp : zbf::object_tracker<mysql_client_resp> {
    uint16_t                version{0};
    mysql_request_id_t      req_id;
    int                     rc;
    DataTables              results;
    FieldsType              fieldsType;

    mysql_client_resp(mysql_request_id_t id) : req_id(id), rc(-1) {
    }

    std::string serialize() const {
        serialize_helper helper;
        helper.serialize_uint16(version);
        helper.serialize_int64(req_id);
        helper.serialize_int32(rc);
        helper.serialize_tables(results);
        helper.serialize_fields_type(fieldsType);
        return helper.data();
    }

    static std::unique_ptr<mysql_client_resp> deserialize(const std::string& data) {
        serialize_helper helper(data);
        mysql_client_resp* resp = new mysql_client_resp(0);
        resp->version = helper.deserialize_uint16();
        resp->req_id = helper.deserialize_int64();
        resp->rc = helper.deserialize_int32();
        resp->results = helper.deserialize_tables();
        resp->fieldsType = helper.deserialize_fields_type();
        return std::unique_ptr<mysql_client_resp>(resp);
    }
};

}
