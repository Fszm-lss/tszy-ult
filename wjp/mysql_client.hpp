#ifndef mysql_client_hpp
#define mysql_client_hpp
// 2026-04

#include <unordered_map>
#include <vector>
#include <list>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>

#include <mysql/mysql.h>
#include <mysql/errmsg.h>

#ifdef _WIN32
// MariaDB my_bool is char, MYSQL_BIND uses my_bool*; needs cast
#define MY_BOOL_CAST(ptr) (my_bool*)(ptr)
#else
// MySQL 8.0+: my_bool removed, MYSQL_BIND uses bool* directly
#define MY_BOOL_CAST(ptr) (ptr)
#endif

#include "mysql_client_req.hpp"
#include "zbf/log_utils.hpp"
#include "zbf/safequeue.hpp"

// sudo apt install libmysqlclient-dev
// link with -lmysqlclient
namespace wjp {

using zbf::LogLevel;

#define MYSQL_ERR_EXIT(rc, code) do { rc = code; goto exit; } while(0)


inline int mysql_client_init() {
    return mysql_library_init(0, nullptr, nullptr);
}

inline void mysql_client_cleanup() {
    mysql_library_end();
}

struct mysql_client_connect_param {
    std::string host;
    std::string user;
    std::string passwd;
    std::string dbname;
    int         port;
    bool        autocommit;
    uint8_t     DBSlotNo{0};
    
    mysql_client_connect_param() : port(3306), autocommit(true) {
    }    
    mysql_client_connect_param(const char* host, const char* user, const char* passwd, const char* dbname, const int port = 3306, bool autocommit = true) 
        : host(host), user(user), passwd(passwd), dbname(dbname), port(port), autocommit(autocommit) {
    }
};

// wrapper opertions for specific database
class mysql_client {
public:
    mysql_client() : _client(nullptr), _connState(false) {
    }

    ~mysql_client() {
        disconnect();
    }

    int connect(const mysql_client_connect_param& param) {
        return connect(param.host.c_str(), param.user.c_str(), param.passwd.c_str(), param.dbname.c_str(), param.port, param.autocommit);
    }

    int connect(const char* host, const char* user, const char* passwd, const char* dbname, const int port = 3306, bool autocommit = true) {
        int rc = -1;
        _client = mysql_init(nullptr);
        if (_client != nullptr) {
            mysql_options(_client, MYSQL_OPT_COMPRESS, nullptr); // enable compress
            mysql_options(_client, MYSQL_READ_DEFAULT_GROUP, "odbc");
            mysql_options(_client, MYSQL_SET_CHARSET_NAME, "utf8mb4");
            unsigned int read_timeout = 10;   // 10 s
            unsigned int write_timeout = 5;   // 5 s
            unsigned int connect_timeout = 5; // 5 s
            mysql_options(_client, MYSQL_OPT_READ_TIMEOUT, &read_timeout);
            mysql_options(_client, MYSQL_OPT_WRITE_TIMEOUT, &write_timeout);
            mysql_options(_client, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);
            // MySQL use autocommit by default, so every SQL is a transaction
            const char* opt = autocommit ? "SET autocommit=1" : "SET autocommit=0";
            mysql_options(_client, MYSQL_INIT_COMMAND, opt);

            if (nullptr != mysql_real_connect(_client, host, user, passwd, dbname, port, nullptr, 0)) {
                rc = 0;
                _connState = true;
            } else {
                mysql_close(_client);
                _client = nullptr;
            }
        }
        return rc;
    }

    void disconnect() {
        if (_client != nullptr) {
            mysql_close(_client); // disconnect DB connection & release resource
            _client = nullptr;
        }
        _connState = false;
    }

public:
    // "create table testdb.student(id int primary key auto_increment, name varchar(128) not null, age int not null, sex varchar(32))"
    // "drop table if exists testdb.student"
    // "insert into testdb.student(name, age, sex) values('Lily', 17, 'female')"
    int execQuery(const char* sql, bool transaction = false) {
        return rawQuery(sql, 0, false, transaction);
	}
    
    int execQuery(const char* sql, unsigned long length, bool transaction = false) {
		if (length == 0) return -1;
        return rawQuery(sql, length, true, transaction);
	}

    // "select * from testdb.student"
    int execQuery(const char* sql, DataTable& table) {
		if (isEmptyString(sql)) return -1;
        if (!_client) return -1;
        int rc = mysql_query(_client, sql);
        if (!rc) {
            MYSQL_RES* result = mysql_store_result(_client);
            if (result) {
                unsigned int num_fields = mysql_num_fields(result);
                // MYSQL_FIELD* fields = mysql_fetch_fields(result);
                MYSQL_ROW row = 0;
                while (row = mysql_fetch_row(result)) {
                    DataRow dataRow;
                    unsigned long* lengths = mysql_fetch_lengths(result);
                    for (int i = 0; i < num_fields; ++i) {
                        if (row[i] == nullptr) {
                            dataRow.push_back(std::string());
                        } else {
                            std::string rawData(row[i], lengths[i]);
                            dataRow.push_back(rawData);
                        }
                    }
                    table.push_back(dataRow);
                }
                mysql_free_result(result);
            } else {
                int err = mysql_errno(_client);
                flushConnState(err);
                LOG_ERR_MSG("mysql_store_result() error: %s(%u), sql=%s", mysql_error(_client), err, sql);
            }
            // consume multi result set to avoid 'Commands out of sync'
            while (!mysql_next_result(_client)) {
                result = mysql_store_result(_client);
                if (result) {
                    mysql_free_result(result);
                }
            }
        } else {
            int err = mysql_errno(_client);
            flushConnState(err);
            LOG_ERR_MSG("mysql_query() error: %s(%u), sql=%s", mysql_error(_client), err, sql);
        }
		return rc;
	}
    
    // No result set version
    int execStatement(const char* sql, const DataRow& param, bool transaction = false) {
        DataTable params;
        params.push_back(param);
        return execStatement(sql, params, param.size(), transaction);
    }

    // No result set version
    int execStatement(const char* sql, const DataTable& params, const int paramCols, bool transaction = false) {
        DataTables results;
        FieldsType fieldsType;
        return execStatement(sql, params, paramCols, 0, results, fieldsType, transaction);
    }

    int execStatement(const char* sql, const DataRow& param, const int resultCols, 
        DataTable& result, FieldsType& fieldsType, bool transaction = false) {
        DataTable params;
        params.push_back(param);
        DataTables results;
        int rc = execStatement(sql, params, param.size(), resultCols, results, fieldsType, transaction);
        if (!rc) {
            if (!results.empty()) {
                result = results[0];
            }
        }
        return rc;
    }

    // "insert into testdb.student(name, age, sex) values(?, ?, ?)"
    // "select testdb.student.name from testdb.student where testdb.student.age >= ?"
    int execStatement(const char* sql, const DataTable& params, const int paramCols, const int resultCols, 
        DataTables& results, FieldsType& fieldsType, bool transaction = false, bool bindOutSimple = true) {
        if (isEmptyString(sql)) return -1;
        if (!_client) return -1;

        int rc = 0;
        int err = 0;
        bool loopCompleted = false;
        MYSQL_STMT* stmt = nullptr;
        bool* stmt_is_nulls = nullptr;
        bool* stmt_errors = nullptr;
        DataTable validParams;

        if (transaction) {
            // "begin" is equal to "start transaction"
            rc = mysql_query(_client, "begin;");
            if (rc) {
                err = mysql_errno(_client);
                flushConnState(err);
                LOG_ERR_MSG("mysql_query() error: %s(%u), sql=%s", mysql_error(_client), err, "begin;");
                MYSQL_ERR_EXIT(rc, -2);
            }
        }
        
        stmt = mysql_stmt_init(_client);
        if (!stmt) {
            LOG_ERR_MSG("mysql_stmt_init() fail, sql=%s", sql);
            MYSQL_ERR_EXIT(rc, -3);
        }

        rc = mysql_stmt_prepare(stmt, sql, strlen(sql));
        if (rc) {
            err = mysql_stmt_errno(stmt);
            flushConnState(err);
            LOG_ERR_MSG("mysql_stmt_prepare() error, %s(%u), sql=%s", mysql_stmt_error(stmt), err, sql);
            MYSQL_ERR_EXIT(rc, -4);
        }
        
        for (int m = 0; m < params.size(); ++m) {
            const DataRow& param = params[m];
            if (param.size() != paramCols) {
                LOG_MSG(LogLevel::Warn, "param exception, param.size()=%d, paramCols=%d, sql=%s", param.size(), paramCols, sql);
            } else {
                validParams.push_back(param);
            }
        }
        if (validParams.empty()) {
            LOG_ERR_MSG("check param failed, no valid param, paramCols=%d, sql=%s", paramCols, sql);
            MYSQL_ERR_EXIT(rc, -11);
        }

        fieldsType.clear();
        for (int m = 0; m < validParams.size(); ++m) {
            mysql_stmt_reset(stmt);
            DataRow& param = validParams[m];

            std::vector<MYSQL_BIND> bind_in(paramCols);
			for (int j = 0; j < param.size(); ++j) {
                memset(&bind_in[j], 0, sizeof(MYSQL_BIND));
				bind_in[j].buffer_type = FIELD_TYPE_STRING;
				bind_in[j].is_null = nullptr;
				bind_in[j].buffer = (void*) param[j].c_str();
				bind_in[j].buffer_length = param[j].size() + 1; // FIELD_TYPE_STRING needs '\0'
			}
			rc = mysql_stmt_bind_param(stmt, bind_in.data());
            if (rc) {
                LOG_ERR_MSG("mysql_stmt_bind_param() error, %s(%u), sql=%s", mysql_stmt_error(stmt), mysql_stmt_errno(stmt), sql);
                MYSQL_ERR_EXIT(rc, -5);
            }

            if (resultCols > 0) {
                std::vector<MYSQL_BIND> bind_out(resultCols);
                std::vector<std::vector<char>> stmt_string_data(resultCols);
                std::vector<unsigned long> real_length(resultCols);
                if (!stmt_is_nulls) stmt_is_nulls = new bool[resultCols];
                if (!stmt_errors) stmt_errors = new bool[resultCols];
                
                if (bindOutSimple)
				    rc = bindStmtOutSimple(stmt, resultCols, bind_out, fieldsType, stmt_string_data, stmt_is_nulls, stmt_errors, real_length);
                else
                    rc = bindStmtOut(stmt, resultCols, bind_out, fieldsType, stmt_string_data, stmt_is_nulls, stmt_errors, real_length);
                if (rc) {
                    LOG_ERR_MSG("bindStmtOutSimple() error, %s(%u), sql=%s", mysql_stmt_error(stmt), mysql_stmt_errno(stmt), sql);
                    MYSQL_ERR_EXIT(rc, -6);
                }

                rc = mysql_stmt_execute(stmt);
                if (rc) {
                    err = mysql_stmt_errno(stmt);
                    flushConnState(err);
                    LOG_ERR_MSG("mysql_stmt_execute() error, %s(%u), sql=%s", mysql_stmt_error(stmt), err, sql);
                    MYSQL_ERR_EXIT(rc, -7);
                }
				
                // To cause the complete result set to be buffered on the client, call mysql_stmt_store_result() after mysql_stmt_bind_result()
                // mysql_stmt_store_result() is optional for result set processing, unless you will call mysql_stmt_data_seek() etc.
                rc = mysql_stmt_store_result(stmt);
                if (rc) {
                    LOG_ERR_MSG("mysql_stmt_store_result() error, %s(%u), sql=%s", mysql_stmt_error(stmt), mysql_stmt_errno(stmt), sql);
                    MYSQL_ERR_EXIT(rc, -8);
                }

                DataTable result;
                int num_rows = mysql_stmt_num_rows(stmt);
			    for (int n = 0; n < num_rows; ++n) {
			        int fetch_rc = mysql_stmt_fetch(stmt);
                    if (fetch_rc == 1) {
                        LOG_ERR_MSG("mysql_stmt_fetch() error, %s(%u), sql=%s", mysql_stmt_error(stmt), mysql_stmt_errno(stmt), sql);
                        MYSQL_ERR_EXIT(rc, -9);
                    } else if (fetch_rc == MYSQL_NO_DATA) {
                        break;
                    } else if (fetch_rc == MYSQL_DATA_TRUNCATED) {
                        LOG_MSG(LogLevel::Warn, "mysql_stmt_fetch() data truncated, sql=%s", sql);
                    }
			        DataRow row;
			        for (int j = 0; j < resultCols; ++j) {
                        if (stmt_is_nulls[j]) {
                            row.push_back(std::string(""));
                        } else {
                            // when MYSQL_DATA_TRUNCATED, real_length overflow
                            unsigned long copy_len = std::min(real_length[j], (unsigned long)stmt_string_data[j].size());
                            row.push_back(std::string((char*)stmt_string_data[j].data(), copy_len));
                        }
                    }
			        result.push_back(row);
			    }
                results.push_back(result);
            } else {
                rc = mysql_stmt_execute(stmt);
                if (rc) {
                    err = mysql_stmt_errno(stmt);
                    flushConnState(err);
                    LOG_ERR_MSG("mysql_stmt_execute() error, %s(%u), sql=%s", mysql_stmt_error(stmt), err, sql);
                    MYSQL_ERR_EXIT(rc, -10);
                }
            }
        }
        loopCompleted = true;
    exit:
        if (stmt) mysql_stmt_close(stmt);
        if (stmt_is_nulls) delete[] stmt_is_nulls;
        if (stmt_errors) delete[] stmt_errors;
        
        // clear result if not success
        if (!loopCompleted || rc != 0) {
            results.clear();
            fieldsType.clear();
        }

        if (transaction && rc != -2) {
            std::string trans = (loopCompleted && 0 == rc) ? "commit;" : "rollback;";
            int trans_rc = mysql_query(_client, trans.c_str());
            if (trans_rc) {
                err = mysql_errno(_client);
                flushConnState(err);
                LOG_ERR_MSG("mysql_query() error: %s(%u), sql=%s", mysql_error(_client), err, trans.c_str());
                if (rc == 0) rc = trans_rc;
            }
        }
        return rc;
    }

    // Set transaction isolation level for current session
    // isolationLevel: "READ-UNCOMMITTED", "READ-COMMITTED", "REPEATABLE-READ", "SERIALIZABLE"
    int setTransactionIsolation(const char* isolationLevel) {
        if (isEmptyString(isolationLevel)) return -1;
        if (!_client) return -1;
        std::string sql("SET SESSION TRANSACTION ISOLATION LEVEL ");
        sql += isolationLevel;
        sql += ";";
        return execQuery(sql.c_str());
    }

    bool getConnState() {
        return _connState;
    }

private:
    bool isEmptyString(const char* str) {
        return (str == nullptr) || (str[0] == 0); // '\0' is 0
    }

    void flushConnState(int err) {
        if (err == CR_CONNECTION_ERROR || err == CR_SERVER_LOST || err == CR_SERVER_GONE_ERROR) {
            _connState = false;
        }
    }

    int rawQuery(const char* sql, unsigned long length, bool real, bool transaction = false) {
		if (isEmptyString(sql)) return -1;
        if (!_client) return -1;
        
        int rc = 0;
        int err = 0;
        if (transaction) {
            // "begin" is equal to "start transaction"
            rc = mysql_query(_client, "begin;");
            if (rc) {
                err = mysql_errno(_client);
                flushConnState(err);
                LOG_ERR_MSG("mysql_query() error: %s(%u), sql=%s", mysql_error(_client), err, "begin;");
                MYSQL_ERR_EXIT(rc, -2);
            }
        }

        if (real)
            rc = mysql_real_query(_client, sql, length);
        else
            rc = mysql_query(_client, sql);

        if (rc) {
            err = mysql_errno(_client);
            flushConnState(err);
            if (real) {
                LOG_ERR_MSG("mysql_real_query() error: %s(%u), length=%u", mysql_error(_client), err, length);
            } else {
                LOG_ERR_MSG("mysql_query() error: %s(%u), sql=%s", mysql_error(_client), err, sql);
            }
        } else {
            // consume result set to avoid 'Commands out of sync'
            MYSQL_RES* result = mysql_store_result(_client);
            if (result) {
                mysql_free_result(result);
            }
            while (!mysql_next_result(_client)) {
                result = mysql_store_result(_client);
                if (result) {
                    mysql_free_result(result);
                }
            }
        }
        
        if (transaction) {
            std::string trans = (0 == rc) ? "commit;" : "rollback;";
            int trans_rc = mysql_query(_client, trans.c_str());
            if (trans_rc) {
                err = mysql_errno(_client);
                flushConnState(err);
                LOG_ERR_MSG("mysql_query() error: %s(%u), sql=%s", mysql_error(_client), err, trans.c_str());
            }
            if (rc == 0) rc = trans_rc;
        }
    exit:
		return rc;
	}

    int bindStmtOut(MYSQL_STMT* stmt, const int resultCols, std::vector<MYSQL_BIND>& bind_out, FieldsType& fieldsType,
        std::vector<std::vector<char>>& string_data, bool* is_nulls, bool* errors, std::vector<unsigned long>& lengths) {

        MYSQL_RES* prepare_meta_result = nullptr;
        prepare_meta_result = mysql_stmt_result_metadata(stmt);
        if (!prepare_meta_result) {
            LOG_ERR_MSG("mysql_stmt_result_metadata() error, %s(%u)", mysql_stmt_error(stmt), mysql_stmt_errno(stmt));
            return -1;
        }

        unsigned int actual_fields = mysql_num_fields(prepare_meta_result);
        if (actual_fields != (unsigned int)resultCols) {
            LOG_ERR_MSG("actual_fields(%d) is not equal resultCols(%d)", actual_fields, resultCols);
            mysql_free_result(prepare_meta_result);
            return -2;
        }

        bool fillFieldsType = fieldsType.empty();
        MYSQL_FIELD* fields = mysql_fetch_fields(prepare_meta_result);
        for (int j = 0; j < resultCols; ++j) {
            memset(&bind_out[j], 0, sizeof(MYSQL_BIND));
            unsigned long buffer_size = 0;
            enum enum_field_types field_type = fields[j].type;
            if (fillFieldsType) fieldsType.push_back(field_type);

            switch (field_type) {
                case MYSQL_TYPE_TINY:
                    buffer_size = sizeof(char);
                    bind_out[j].buffer_type = MYSQL_TYPE_TINY;
                    break;
                case MYSQL_TYPE_SHORT:
                    buffer_size = sizeof(short);
                    bind_out[j].buffer_type = MYSQL_TYPE_SHORT;
                    break;
                case MYSQL_TYPE_LONG:
                    buffer_size = sizeof(int32_t);
                    bind_out[j].buffer_type = MYSQL_TYPE_LONG;
                    break;
                case MYSQL_TYPE_LONGLONG:
                    buffer_size = sizeof(int64_t);
                    bind_out[j].buffer_type = MYSQL_TYPE_LONGLONG;
                    break;
                case MYSQL_TYPE_FLOAT:
                    buffer_size = sizeof(float);
                    bind_out[j].buffer_type = MYSQL_TYPE_FLOAT;
                    break;
                case MYSQL_TYPE_DOUBLE:
                    buffer_size = sizeof(double);
                    bind_out[j].buffer_type = MYSQL_TYPE_DOUBLE;
                    break;
                case MYSQL_TYPE_DATE:
                case MYSQL_TYPE_TIME:
                case MYSQL_TYPE_DATETIME:
                    buffer_size = sizeof(MYSQL_TIME);
                    bind_out[j].buffer_type = field_type;
                    break;
                case MYSQL_TYPE_BLOB:
                case MYSQL_TYPE_LONG_BLOB:
                case MYSQL_TYPE_MEDIUM_BLOB:
                case MYSQL_TYPE_TINY_BLOB:
                    buffer_size = (fields[j].length > 0 && fields[j].length < MAX_BLOB_SIZE) ? fields[j].length : MAX_BLOB_SIZE;
                    bind_out[j].buffer_type = MYSQL_TYPE_BLOB;
                    break;
                default:
                    // MYSQL_TYPE_VARCHAR / MYSQL_TYPE_STRING / MYSQL_TYPE_VAR_STRING / MYSQL_TYPE_JSON
                    buffer_size = (fields[j].length > 0 && fields[j].length < MAX_STRING_SIZE) ? fields[j].length + 1 : MAX_STRING_SIZE;
                    bind_out[j].buffer_type = MYSQL_TYPE_STRING;
                    break;
            }

            string_data[j].resize(buffer_size);
            is_nulls[j] = false;
            errors[j] = false;
            lengths[j] = 0;

            bind_out[j].buffer = string_data[j].data();
            bind_out[j].buffer_length = buffer_size;
            bind_out[j].is_null = MY_BOOL_CAST(&is_nulls[j]);
            bind_out[j].length = &lengths[j];
            bind_out[j].error = MY_BOOL_CAST(&errors[j]);
        }

        mysql_free_result(prepare_meta_result);
        return mysql_stmt_bind_result(stmt, bind_out.data());
    }

    int bindStmtOutSimple(MYSQL_STMT* stmt, const int resultCols, std::vector<MYSQL_BIND>& bind_out, FieldsType& fieldsType,
        std::vector<std::vector<char>>& string_data, bool* is_nulls, bool* errors, std::vector<unsigned long>& lengths) {

        MYSQL_RES* prepare_meta_result = nullptr;
        prepare_meta_result = mysql_stmt_result_metadata(stmt);
        if (!prepare_meta_result) {
            LOG_ERR_MSG("mysql_stmt_result_metadata() error, %s(%u)", mysql_stmt_error(stmt), mysql_stmt_errno(stmt));
            return -1;
        }

        unsigned int actual_fields = mysql_num_fields(prepare_meta_result);
        if (actual_fields != (unsigned int) resultCols) {
            LOG_ERR_MSG("actual_fields(%d) is not equal resultCols(%d)", actual_fields, resultCols);
            mysql_free_result(prepare_meta_result);
            return -2;
        }

        bool fillFieldsType = fieldsType.empty();
        MYSQL_FIELD* fields = mysql_fetch_fields(prepare_meta_result);
        for (int j = 0; j < resultCols; ++j) {
            memset(&bind_out[j], 0, sizeof(MYSQL_BIND));
            unsigned long buffer_size = 0;
            enum enum_field_types field_type = fields[j].type;
            if (fillFieldsType) fieldsType.push_back(field_type);

            switch (field_type) {
                case MYSQL_TYPE_VARCHAR:
                case MYSQL_TYPE_STRING:
                case MYSQL_TYPE_VAR_STRING:
                case MYSQL_TYPE_JSON:
                    buffer_size = (fields[j].length > 0 && fields[j].length < MAX_STRING_SIZE) ? fields[j].length + 1 : MAX_STRING_SIZE;
                    bind_out[j].buffer_type = MYSQL_TYPE_STRING;
                    break;
                case MYSQL_TYPE_BLOB:
                case MYSQL_TYPE_LONG_BLOB:
                case MYSQL_TYPE_MEDIUM_BLOB:
                case MYSQL_TYPE_TINY_BLOB:
                    buffer_size = (fields[j].length > 0 && fields[j].length < MAX_BLOB_SIZE) ? fields[j].length : MAX_BLOB_SIZE;
                    bind_out[j].buffer_type = MYSQL_TYPE_BLOB;
                    break;
                default:
                    // numerical
                    buffer_size = 32; // 32
                    bind_out[j].buffer_type = MYSQL_TYPE_STRING;
                    break;
            }

            string_data[j].resize(buffer_size);
            is_nulls[j] = false;
            errors[j] = false;
            lengths[j] = 0;

            bind_out[j].buffer = string_data[j].data();
            bind_out[j].buffer_length = buffer_size;
            bind_out[j].is_null = MY_BOOL_CAST(&is_nulls[j]);
            bind_out[j].length = &lengths[j];
            bind_out[j].error = MY_BOOL_CAST(&errors[j]);
        }

        mysql_free_result(prepare_meta_result);
        return mysql_stmt_bind_result(stmt, bind_out.data());
    }

private:
    MYSQL* _client;
    std::atomic<bool> _connState;

    enum { MAX_STRING_SIZE = 65536/*64K*/, MAX_BLOB_SIZE = 16*1048576/*16M*/ };
};


class mysql_client_pool_listener {
public:
    virtual void onResponse(const mysql_client_req* req, const mysql_client_resp* resp) = 0;
};


class mysql_client_pool {
public:
    mysql_client_pool(const mysql_client_connect_param& param, mysql_client_pool_listener* listener)
        : _reqId(MinReqId), _poolSize(0), _connParam(param), _listener(listener) {
    }

    virtual ~mysql_client_pool() {
    }

    int start(int poolSize = PoolSize) {
        poolSize = std::max(poolSize, 1);
        poolSize = std::min(poolSize, 16);

        _poolSize = poolSize;
        int workerNum = poolSize;
        for (int i = 1; i <= workerNum; ++i) {
            std::thread* thd = new std::thread([&, i](){
                short thdId = ThdIdBase+i;
                worker(thdId);
            });
            _workers.push_back(thd);
        }
        LOG_MSG(LogLevel::Debug, "%s start, pool size=%d", desc().c_str(), workerNum);
        return 0;
    }

    void serveUtilStop() {
        for (int i = 0; i < _workers.size(); ++i) {
            std::thread* thd = _workers[i];
            if (thd->joinable())
                thd->join();
            delete thd;
        }
        _workers.clear();

        std::unique_ptr<mysql_client_req> req;
        while (_queue.raw_pop(req)) {}

        uninitPool();
        LOG_MSG(LogLevel::Debug, "%s stop", desc().c_str());
    }

    void stop(bool join = false) {
        _queue.stop();
        if (join) serveUtilStop();
    }

    mysql_request_id_t submit(std::unique_ptr<mysql_client_req> req) {
        mysql_request_id_t rc = genRequestId();
        req->req_id = rc;
        if (_queue.push(std::move(req))) {
            return rc;
        }
        LOG_ERR_MSG("submit fail: pool busy, reqId=%lu", rc);
        return PoolIsBusy;
    }

    // run in caller thread
    int execQuery(const char* sql, bool transaction = false) {
        int rc = -1;
        mysql_client* client = getIdleClient();
        if (client) {
            rc = client->execQuery(sql, transaction);
            if (!client->getConnState()) { // not connected
                closeClient(client);
            } else {
                markClientIdle(client);
            }
        }
        return rc;
    }

    const mysql_client_connect_param& connectParam() {
        return _connParam;
    }

    std::string desc() {
        return std::string("mysql_client_pool-") + _connParam.dbname;
    }

private:
    mysql_request_id_t genRequestId() {
        return _reqId.fetch_add(1, std::memory_order_relaxed);
    }

    void uninitPool() {
        std::unordered_map<mysql_client*, bool>::iterator it = _clt2idle.begin();
        for ( ; it != _clt2idle.end(); ++it) {
            mysql_client* clt = it->first;
            clt->disconnect();
            delete clt;
        }
        _clt2idle.clear();
    }    

    void handleRequest(mysql_client* client, std::unique_ptr<mysql_client_req> req) {
        auto resp = std::make_unique<mysql_client_resp>(req->req_id);
        switch (req->type) {
            case mysql_client_req_type::execQuery:
                resp->rc = client->execQuery(req->sql.c_str(), req->transaction);
                _listener->onResponse(req.get(), resp.get());
                break;
            case mysql_client_req_type::execRealQuery:
                resp->rc = client->execQuery(req->sql.data(), req->sql.size(), req->transaction);
                _listener->onResponse(req.get(), resp.get());
                break;
            case mysql_client_req_type::execQueryForResult: {
                DataTable result;
                resp->rc = client->execQuery(req->sql.c_str(), result);
                if (!resp->rc) resp->results.push_back(result);
                _listener->onResponse(req.get(), resp.get());
                break;
            }
            case mysql_client_req_type::execStatement: {
                mysql_stmt_req_params* stmt_param = req->stmt_param;
                if (stmt_param != nullptr) {
                    resp->rc = client->execStatement(req->sql.c_str(), stmt_param->params, stmt_param->paramCols, stmt_param->resultCols, 
                        resp->results, resp->fieldsType, req->transaction);
                } else {
                    LOG_ERR_MSG("request stmt_param invalid, req=%s", req->desc().c_str());
                    resp->rc = RESP_RC_PARAM_INVALID;
                }
                _listener->onResponse(req.get(), resp.get());
                break;
            }
            default: {
                LOG_ERR_MSG("request type invalid, req=%s", req->desc().c_str());
                resp->rc = RESP_RC_TYPE_INVALID;
                _listener->onResponse(req.get(), resp.get());
                break;
            }
        }
    }

    void retryFailure(std::unique_ptr<mysql_client_req> req) {
        LOG_ERR_MSG("request fail, no available client, req=%s", req->desc().c_str());
        auto resp = std::make_unique<mysql_client_resp>(req->req_id);
        resp->rc = RESP_RC_NO_AVAIL_CLT;
        _listener->onResponse(req.get(), resp.get());
    }

    void worker(short thdId) {
        LOG_MSG(LogLevel::Debug, "%s worker thread(%d) start", desc().c_str(), thdId);

        for (;;) {
            std::unique_ptr<mysql_client_req> req;
            int rc = _queue.pop_timeout(req, 100);
            if (rc == zbf::SQ_POP_SUCCESS) {
                mysql_client* client = getIdleClient();
                if (client && client->getConnState()) {
                    handleRequest(client, std::move(req));
                    if (client->getConnState()) {
                        markClientIdle(client);
                    } else {
                        closeClient(client);
                    }
                } else {
                    if (client) closeClient(client);

                    if (req->retry_times >= MaxRetryTimes) { 
                        // already re-queue retry_times
                        retryFailure(std::move(req));
                    } else {
                        // no available client, re-queue request
                        int retry_times = ++req->retry_times;
                        _queue.requeue(std::move(req));
                        int wait_ms = 50 * std::pow(2, retry_times-1);
                        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
                    }
                }
            } else if (rc == zbf::SQ_EXIT) {
                break;
            }
        }
        LOG_MSG(LogLevel::Debug, "%s worker thread(%d) exit", desc().c_str(), thdId);
    }

    mysql_client* getIdleClient() {
        mysql_client* clt = nullptr;
        mysql_client* newClt = nullptr;
        {
            std::lock_guard<std::mutex> lock(_lockClients);
            auto it = _clt2idle.begin();
            for (; it != _clt2idle.end(); ++it) {
                if (it->second) {
                    it->second = false;
                    clt = it->first;
                    break;
                }
            }
            if (!clt && _clt2idle.size() < _poolSize) {
                newClt = new mysql_client();
                _clt2idle.insert(std::make_pair(newClt, false));
            }
        }
        if (!clt && newClt) {
            int rc = newClt->connect(_connParam);
            std::lock_guard<std::mutex> lock(_lockClients);
            if (0 == rc) {
                clt = newClt;
            } else {
                // LOG_ERR_MSG("%s connect fail, rc=%d", desc().c_str(), rc);
                _clt2idle.erase(newClt);
                newClt->disconnect();
                delete newClt;
                newClt = nullptr;
            }
        }
        return clt;
    }

    void markClientIdle(mysql_client* clt) {
        std::lock_guard<std::mutex> lock(_lockClients);
        auto it = _clt2idle.find(clt);
        if (it != _clt2idle.end()) {
            it->second = true;
        }
    }

    void closeClient(mysql_client* clt) {
        std::lock_guard<std::mutex> lock(_lockClients);
        _clt2idle.erase(clt);
        clt->disconnect();
        delete clt;
    }

public:
    enum { PoolSize = 10, ThdIdBase = 600, MaxPendingReq = 256, MinReqId = 100, PoolIsBusy = 1, MaxRetryTimes = 3 };
    enum { RESP_RC_PARAM_INVALID = 1001, RESP_RC_TYPE_INVALID = 1002, RESP_RC_NO_AVAIL_CLT = 1003 };

private:
    std::vector<std::thread *> _workers;
    std::atomic<mysql_request_id_t> _reqId;
    zbf::SafeQueue<std::unique_ptr<mysql_client_req>, MaxPendingReq> _queue;
    std::unordered_map<mysql_client*, bool> _clt2idle;
    std::mutex _lockClients;
    int _poolSize;
    mysql_client_connect_param _connParam;
    mysql_client_pool_listener* _listener; // ref, no need lock
};


}

#endif
