#ifndef mongo_client_hpp
#define mongo_client_hpp
// 2025-06

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "mongo_client_req.hpp"
#include "zbf/log_utils.hpp"
#include "zbf/safequeue.hpp"

// -I mongocdriver2.0/include -L mongocdriver2.0/lib -lbson2 -lmongoc2
namespace wjp {

using zbf::LogLevel;

class mongo_client;
typedef bool (*TRANSACTION_PROCEDURE)(mongo_client* dbClt, void* param);

inline void mongo_client_init() {
    mongoc_init();
}

inline void mongo_client_cleanup() {
    mongoc_cleanup();
}

// wrapper opertions for specific database
class mongo_client {
public:
    mongo_client(int seq) : _seq(seq) {
        _client = nullptr;
        _database = nullptr;
    }

    virtual ~mongo_client() {
    }

    int seq() {
        return _seq;
    }
    
    int connect(const char* conn_uri) {
        _client = mongoc_client_new(conn_uri);
        return (_client == nullptr) ? -1 : 0;
    }
    void disconnect() {
        if (_client) {
            mongoc_client_destroy(_client);
            _client = nullptr;
        }
    }

    void attach(mongoc_client_t* clt) {
        _client = clt;
    }
    int attach(mongoc_client_t* clt, const char* db_name) {
        _client = clt;
        return open(db_name);
    }

    void detach() {
        close();
        _client = nullptr;
    }

    int open(const char* db_name) {
        if (db_name == nullptr) return -1;
        if (_client) _database = mongoc_client_get_database(_client, db_name);
        return (_database == nullptr) ? -1 : 0;
    }
    void close() {
        clearCachedCollections();
        if (_database) {
            mongoc_database_destroy(_database);
            _database = nullptr;
        }
    }

public:
    // database
    // Databases are automatically created on the MongoDB server upon insertion of the first document into a collection. 
    // There is no need to create a database manually.
    int dropDatabase() {
        int rc = 0;
        bson_error_t err;
        if (!mongoc_database_drop(_database, &err)) {
            rc = -1;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
        return rc;
    }

    // collection
    // Collections are automatically created on the MongoDB server upon insertion of the first document.
    // There is no need to create a collection manually.
    int dropCollection(const char* collName) {
        int rc = 0;
        bson_error_t err;
        mongoc_collection_t* coll = getCollection(collName);
        if (!mongoc_collection_drop(coll, &err)) {
            rc = -1;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
        return rc;
    }

    // CRUD
    int insertOne(const char* collName, const bson_t* doc, const bson_t* opts = nullptr, bson_t* reply = nullptr) {
        int rc = 0;
        bson_error_t err;
        mongoc_collection_t* coll = getCollection(collName);
        if (!mongoc_collection_insert_one(coll, doc, opts, reply, &err)) {
            rc = -1;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
        return rc;
    }

    int insertMany(const char* collName, const bson_t** docs, size_t doc_cnt, const bson_t* opts = nullptr, bson_t* reply = nullptr) {
        int rc = 0;
        bson_error_t err;
        mongoc_collection_t* coll = getCollection(collName);
        if (!mongoc_collection_insert_many(coll, docs, doc_cnt, opts, reply, &err)) {
            rc = -1;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
        return rc;
    }

    int find(const char* collName, const bson_t* query, std::vector<bson_ptr>& docs, const bson_t* opts = nullptr, const mongoc_read_prefs_t* read_prefs = nullptr) {
        int rc = 0;
        bson_error_t cursor_err;
        const bson_t* doc;
        mongoc_collection_t* coll = getCollection(collName);
        mongoc_cursor_t* results = mongoc_collection_find_with_opts(coll, query, opts, read_prefs);
        if (!results) {
            rc = -1;
            LOG_ERR_MSG("error: mongoc_collection_find_with_opts returned null");
            goto exit;
        }
        docs.clear();
        while (mongoc_cursor_next(results, &doc)) {
            docs.emplace_back(bson_copy(doc));
        }
        if (mongoc_cursor_error(results, &cursor_err)) {
            rc = -2;
            LOG_ERR_MSG("cursor error: %s(%u)", cursor_err.message, cursor_err.code);
        }
    exit:
        if (results) mongoc_cursor_destroy(results);
        return rc;
    }

    int find(const char* collName, const bson_t* query, std::vector<std::string>& docs, const bson_t* opts = nullptr, const mongoc_read_prefs_t* read_prefs = nullptr) {
        int rc = 0;
        bson_error_t cursor_err;
        const bson_t* doc;
        mongoc_collection_t* coll = getCollection(collName);
        mongoc_cursor_t* results = mongoc_collection_find_with_opts(coll, query, opts, read_prefs);
        if (!results) {
            rc = -1;
            LOG_ERR_MSG("error: mongoc_collection_find_with_opts returned null");
            goto exit;
        }
        docs.clear();
        while (mongoc_cursor_next(results, &doc)) {
            // char* json_doc = bson_as_canonical_extended_json(doc, nullptr);
            char* json_doc = bson_as_legacy_extended_json(doc, nullptr);
            docs.push_back(std::string(json_doc));
            bson_free(json_doc);
        }
        if (mongoc_cursor_error(results, &cursor_err)) {
            rc = -2;
            LOG_ERR_MSG("cursor error: %s(%u)", cursor_err.message, cursor_err.code);
        }
    exit:
        if (results) mongoc_cursor_destroy(results);
        return rc;
    }

    int updateOne(const char* collName, const bson_t* query, const bson_t* update, const bson_t* opts = nullptr, bson_t* reply = nullptr) {
        int rc = 0;
        bson_error_t err;
        mongoc_collection_t* coll = getCollection(collName);
        if (!mongoc_collection_update_one(coll, query, update, opts, reply, &err)) {
            rc = -1;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
        return rc;
    }

    int updateMany(const char* collName, const bson_t* query, const bson_t* update, const bson_t* opts = nullptr, bson_t* reply = nullptr) {
        int rc = 0;
        bson_error_t err;
        mongoc_collection_t* coll = getCollection(collName);
        if (!mongoc_collection_update_many(coll, query, update, opts, reply, &err)) {
            rc = -1;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
        return rc;
    }

    int replaceOne(const char* collName, const bson_t* query, const bson_t* replace, const bson_t* opts = nullptr, bson_t* reply = nullptr) {
        int rc = 0;
        bson_error_t err;
        mongoc_collection_t* coll = getCollection(collName);
        if (!mongoc_collection_replace_one(coll, query, replace, opts, reply, &err)) {
            rc = -1;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
        return rc;
    }

    int deleteOne(const char* collName, const bson_t* query, const bson_t* opts = nullptr, bson_t* reply = nullptr) {
        int rc = 0;
        bson_error_t err;
        mongoc_collection_t* coll = getCollection(collName);
        if (!mongoc_collection_delete_one(coll, query, opts, reply, &err)) {
            rc = -1;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
        return rc;
    }

    int deleteMany(const char* collName, const bson_t* query, const bson_t* opts = nullptr, bson_t* reply = nullptr) {
        int rc = 0;
        bson_error_t err;
        mongoc_collection_t* coll = getCollection(collName);
        if (!mongoc_collection_delete_many(coll, query, opts, reply, &err)) {
            rc = -1;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
        return rc;
    }

    // command
    int command(const bson_t* cmd, const mongoc_read_prefs_t* read_prefs = nullptr, const bson_t* opts = nullptr, bson_t* reply = nullptr) {
        int rc = 0;
        bson_error_t err;
        const char* dbName = mongoc_database_get_name(_database);
        if (!mongoc_client_command_with_opts(_client, dbName, cmd, read_prefs, opts, reply, &err)) {
            rc = -1;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
        return rc;
    }

    int commandSimple(const bson_t* cmd, const mongoc_read_prefs_t* read_prefs = nullptr, bson_t* reply = nullptr) {
        int rc = 0;
        bson_error_t err;
        const char* dbName = mongoc_database_get_name(_database);
        if (!mongoc_client_command_simple(_client, dbName, cmd, read_prefs, reply, &err)) {
            rc = -1;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
        return rc;
    }

    // bulk operations
    mongoc_bulk_operation_t* createBulk(const char* collName, const bson_t* opts = nullptr) {
        mongoc_collection_t* coll = getCollection(collName);
        mongoc_bulk_operation_t* bulk = mongoc_collection_create_bulk_operation_with_opts(coll, opts);
        return bulk;
    }

    int bulkExecute(mongoc_bulk_operation_t* bulk, bson_t* reply = nullptr) {
        int rc = 0;
        bson_error_t err;
        if (!mongoc_bulk_operation_execute(bulk, reply, &err)) {
            rc = -1;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
        return rc;
    }

    void destroyBulk(mongoc_bulk_operation_t*& bulk) {
        if (bulk) {
            mongoc_bulk_operation_destroy(bulk);
            bulk = nullptr;
        }
    }

    int bulkInsert(mongoc_bulk_operation_t* bulk, const bson_t* doc, const bson_t* opts = nullptr) {
        int rc = 0;
        bson_error_t err;
        if (!mongoc_bulk_operation_insert_with_opts(bulk, doc, opts, &err)) {
            rc = -1;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
        return rc;
    }    
    
    int bulkUpdateOne(mongoc_bulk_operation_t* bulk, const bson_t* query, const bson_t* doc, const bson_t* opts = nullptr) {
        int rc = 0;
        bson_error_t err;
        if (!mongoc_bulk_operation_update_one_with_opts(bulk, query, doc, opts, &err)) {
            rc = -1;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
        return rc;
    }

    int bulkUpdateMany(mongoc_bulk_operation_t* bulk, const bson_t* query, const bson_t* doc, const bson_t* opts = nullptr) {
        int rc = 0;
        bson_error_t err;
        if (!mongoc_bulk_operation_update_many_with_opts(bulk, query, doc, opts, &err)) {
            rc = -1;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
        return rc;
    }

    int bulkReplaceOne(mongoc_bulk_operation_t* bulk, const bson_t* query, const bson_t* doc, const bson_t* opts = nullptr) {
        int rc = 0;
        bson_error_t err;
        if (!mongoc_bulk_operation_replace_one_with_opts(bulk, query, doc, opts, &err)) {
            rc = -1;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
        return rc;
    }

    int bulkRemoveOne(mongoc_bulk_operation_t* bulk, const bson_t* query, const bson_t* opts = nullptr) {
        int rc = 0;
        bson_error_t err;
        if (!mongoc_bulk_operation_remove_one_with_opts(bulk, query, opts, &err)) {
            rc = -1;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
        return rc;
    }

    int bulkRemoveMany(mongoc_bulk_operation_t* bulk, const bson_t* query, const bson_t* opts = nullptr) {
        int rc = 0;
        bson_error_t err;
        if (!mongoc_bulk_operation_remove_many_with_opts(bulk, query, opts, &err)) {
            rc = -1;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
        return rc;
    }

    // transactions
    mongoc_client_session_t* createSession(const mongoc_session_opt_t* opts = nullptr) {
        bson_error_t err;
        mongoc_client_session_t* session = mongoc_client_start_session(_client, opts, &err);
        if (!session) {
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
        return session;
    }

    void destroySession(mongoc_client_session_t*& session) {
        if (session) {
            mongoc_client_session_destroy(session);
            session = nullptr;
        }
    }

    int startTransaction(mongoc_client_session_t* session, const mongoc_transaction_opt_t* opts = nullptr) {
        int rc = 0;
        bson_error_t err;
        if (!mongoc_client_session_start_transaction(session, opts, &err)) {
            rc = -1;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
        return rc;
    }

    int commitTransaction(mongoc_client_session_t* session, bson_t* reply = nullptr) {
        int rc = 0;
        bson_error_t err;
        if (!mongoc_client_session_commit_transaction(session, reply, &err)) {
            rc = -1;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
        return rc;
    }

    int rollbackTransaction(mongoc_client_session_t* session) {
        int rc = 0;
        bson_error_t err;
        if (!mongoc_client_session_abort_transaction(session, &err)) {
            rc = -1;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
        return rc;
    }

    int execTransaction(TRANSACTION_PROCEDURE trans_proc, void* param, const mongoc_session_opt_t* sopts = nullptr, const mongoc_transaction_opt_t* topts = nullptr, bson_t* reply = nullptr) {
        int rc = 0;
        bool exec_success = false;
        mongoc_client_session_t* session = createSession(sopts);
        if (!session) {
            rc = -1;
            goto exit;
        }
        // start
        if (startTransaction(session, topts)) {
            rc = -2;
            goto exit;
        }
        // execute
        exec_success = trans_proc(this, param);
        // commit
        if (exec_success) {
            if (commitTransaction(session, reply)) {
                exec_success = false;
            }
        }
        // rollback
        if (!exec_success) {
            if (rollbackTransaction(session)) {
                rc = -3;
            } else {
                rc = -4; // transaction is rollback
            }
        }
    exit:
        destroySession(session);
        return rc;
    }
    
    int execTransaction(mongoc_client_session_with_transaction_cb_t cb, const mongoc_session_opt_t* sopts = nullptr, const mongoc_transaction_opt_t *topts = nullptr, bson_t* reply = nullptr) {
        int rc = 0;
        bson_error_t err;
        bool exec_success = false;
        mongoc_client_session_t* session = createSession(sopts);
        if (!session) {
            rc = -1;
            goto exit;
        }
        // execute
        exec_success = mongoc_client_session_with_transaction(session, cb, topts, this, reply, &err);
        if (!exec_success) {
            rc = -2;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
        }
    exit:
        destroySession(session);
        return rc;
    }

    // index, does not implement
    // mongoc_collection_create_indexes_with_opts
    // mongoc_collection_drop_index

    // prefs
    void setDBReadPrefs(const mongoc_read_mode_t mode, const bson_t *tags = nullptr, int64_t max_staleness_seconds = 0L) {
        mongoc_read_prefs_t* prefs = mongoc_read_prefs_new(mode);
        if (tags) mongoc_read_prefs_set_tags(prefs, tags);
        if (max_staleness_seconds == MONGOC_NO_MAX_STALENESS || max_staleness_seconds >= MONGOC_SMALLEST_MAX_STALENESS_SECONDS) 
            mongoc_read_prefs_set_max_staleness_seconds(prefs, max_staleness_seconds);
        mongoc_database_set_read_prefs(_database, prefs);
        mongoc_read_prefs_destroy(prefs);
    }

    void setCollReadPrefs(const char* collName, const mongoc_read_mode_t mode, const bson_t *tags = nullptr, int64_t max_staleness_seconds = 0L) {
        mongoc_collection_t* coll = getCollection(collName);
        mongoc_read_prefs_t* prefs = mongoc_read_prefs_new(mode);
        if (tags) mongoc_read_prefs_set_tags(prefs, tags);
        if (max_staleness_seconds == MONGOC_NO_MAX_STALENESS || max_staleness_seconds >= MONGOC_SMALLEST_MAX_STALENESS_SECONDS) 
            mongoc_read_prefs_set_max_staleness_seconds(prefs, max_staleness_seconds);
        mongoc_collection_set_read_prefs(coll, prefs);
        mongoc_read_prefs_destroy(prefs);
    }
    
    // concern, does not implement
    // mongoc_database_set_read_concern()
    // mongoc_database_set_write_concern()
    // mongoc_collection_set_read_concern()
    // mongoc_collection_set_write_concern()

private:
    mongoc_collection_t* getCollection(const char* collName) {
        mongoc_collection_t* coll = nullptr;
        auto it = _name2coll.find(std::string(collName));
        if (it != _name2coll.end()) {
            coll = it->second;
        } else {
            coll = mongoc_database_get_collection(_database, collName);
            _name2coll.insert(std::make_pair(std::string(collName), coll));
        }
        return coll;
    }

    void clearCachedCollections() {
        auto it = _name2coll.begin();
        for (; it != _name2coll.end(); ++it) {
            mongoc_collection_t* coll = it->second;
            mongoc_collection_destroy(coll);
        }
        _name2coll.clear();
    }

private:
    mongoc_client_t* _client;
    mongoc_database_t* _database;
    // Not a cross-request cache — cleared every attach/detach cycle.
    // Kept as a map to deduplicate collection objects and defer destroy
    // until close(), because mongoc bulk ops hold raw pointers to them.
    std::unordered_map<std::string, mongoc_collection_t*> _name2coll;
    int _seq;
};


class mongo_client_pool_listener {
public:
    virtual void onResponse(const mongo_client_req* req, const mongo_client_resp* resp) = 0;
};

struct mongo_client_connect_param {
    std::string conn_uri;
    std::string dbname;
    uint8_t DBSlotNo{0};
};

class mongo_client_pool {
public:
    mongo_client_pool(const mongo_client_connect_param& param, mongo_client_pool_listener* listener)
        : _connParam(param), _listener(listener), _poolSize(0), _pool(nullptr), _cltSeq(0), _reqId(MinReqId) {
    }

    virtual ~mongo_client_pool() {
    }

    int start(int poolSize = PoolSize) {
        poolSize = std::max(poolSize, 1);
        poolSize = std::min(poolSize, 16);

        int rc = initPool(poolSize);
        if (rc) return rc;

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

        // Drain remaining requests (unique_ptr auto-deletes)
        std::unique_ptr<mongo_client_req> req;
        while (_queue.raw_pop(req)) {}

        uninitPool();
        LOG_MSG(LogLevel::Debug, "%s stop", desc().c_str());
    }

    void stop(bool join = false) {
        _queue.stop();
        if (join) serveUtilStop();
    }

    mongo_request_id_t submit(std::unique_ptr<mongo_client_req> req) {
        mongo_request_id_t rc = genRequestId();
        req->req_id = rc;
        if (_queue.push(std::move(req))) {
            return rc;
        }
        LOG_ERR_MSG("submit fail: pool busy, reqId=%lu", rc);
        return PoolIsBusy;
    }

    const mongo_client_connect_param& connectParam() {
        return _connParam;
    }

    std::string desc() {
        return std::string("mongo_client_pool-") + _connParam.dbname;
    }

private:
    mongo_request_id_t genRequestId() {
        return _reqId.fetch_add(1, std::memory_order_relaxed);
    }

    int initPool(int poolSize) {
        int rc = 0;
        bson_error_t err;
        mongoc_server_api_t* api = nullptr;
        _poolSize = poolSize;

        // use &waitQueueTimeoutMS=1000 set pool pop timeout
        mongoc_uri_t* uri = mongoc_uri_new_with_error(_connParam.conn_uri.c_str(), &err);
        if (!uri) {
            rc = -1;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
            goto exit;
        }
        _pool = mongoc_client_pool_new(uri);
        if (!_pool) {
            rc = -2;
            LOG_ERR_MSG("error: mongoc_client_pool_new()");
            goto exit;
        }
        mongoc_client_pool_max_size(_pool, _poolSize);
        mongoc_client_pool_set_error_api(_pool, 2);
        api = mongoc_server_api_new(MONGOC_SERVER_API_V1);
        if (!mongoc_client_pool_set_server_api(_pool, api, &err)) {
            rc = -3;
            LOG_ERR_MSG("error: %s(%u)", err.message, err.code);
            goto exit;
        }

    exit:
        if (api) mongoc_server_api_destroy(api);
        if(uri) mongoc_uri_destroy(uri);
        if (rc && _pool) {
            mongoc_client_pool_destroy(_pool);
            _pool = nullptr;
        }
        return rc;
    }

    void uninitPool() {
        if (_pool) mongoc_client_pool_destroy(_pool);
        auto it = _clt2idle.begin();
        for ( ; it != _clt2idle.end(); ++it) {
            mongo_client* dbClt = it->first;
            delete dbClt;
        }
        _clt2idle.clear();
    }

    static bool transaction_proc(mongo_client* dbClt, void* param) {
        mongo_client_req* head = (mongo_client_req*) param;
        int rc = 0;
        mongo_client_req* req = head;
        while (req->next != nullptr) {
            req = req->next.get();
            switch (req->type) {
                case mongo_client_req_type::insertOne:
                    rc = dbClt->insertOne(req->collName.c_str(), req->write, req->opts, nullptr);
                    break;
                case mongo_client_req_type::insertMany:
                    rc = dbClt->insertMany(req->collName.c_str(), (const bson_t**)req->mult_write, req->mwrite_size, req->opts, nullptr);
                    break;
                case mongo_client_req_type::updateOne:
                    rc = dbClt->updateOne(req->collName.c_str(), req->query, req->write, req->opts, nullptr);
                    break;
                case mongo_client_req_type::updateMany:
                    rc = dbClt->updateMany(req->collName.c_str(), req->query, req->write, req->opts, nullptr);
                    break;
                case mongo_client_req_type::replaceOne:
                    rc = dbClt->replaceOne(req->collName.c_str(), req->query, req->write, req->opts, nullptr);
                    break;
                case mongo_client_req_type::deleteOne:
                    rc = dbClt->deleteOne(req->collName.c_str(), req->query, req->opts, nullptr);
                    break;
                case mongo_client_req_type::deleteMany:
                    rc = dbClt->deleteMany(req->collName.c_str(), req->query, req->opts, nullptr);
                    break;
                case mongo_client_req_type::bulkExecute: // not support now
                    rc = RESP_RC_NOT_SUPPORT;
                    break;
                default:
                    rc = RESP_RC_TYPE_INVALID;
                    break;
            }
            if (rc) break;
        }
        return (0 == rc);
    }

    int bulkExecute(mongo_client* dbClt, mongo_client_req* head, mongo_client_resp* resp) {
        int rc = 0;
        mongo_client_req* req = head;
        mongoc_bulk_operation_t* bulk = dbClt->createBulk(req->collName.c_str(), req->opts);
        if (!bulk) return RESP_RC_PARAM_INVALID;

        while (req->next != nullptr) {
            req = req->next.get();
            switch (req->type) {
                case mongo_client_req_type::insertOne:
                    rc = dbClt->bulkInsert(bulk, req->write, req->opts);
                    break;
                case mongo_client_req_type::insertMany: {
                    for (int i = 0; i < req->mwrite_size; ++i) {
                        rc = dbClt->bulkInsert(bulk, req->mult_write[i], req->opts);
                        if (rc) break;
                    }
                    break;
                }
                case mongo_client_req_type::updateOne:
                    rc = dbClt->bulkUpdateOne(bulk, req->query, req->write, req->opts);
                    break;
                case mongo_client_req_type::updateMany:
                    rc = dbClt->bulkUpdateMany(bulk, req->query, req->write, req->opts);
                    break;
                case mongo_client_req_type::replaceOne:
                    rc = dbClt->bulkReplaceOne(bulk, req->query, req->write, req->opts);
                    break;
                case mongo_client_req_type::deleteOne:
                    rc = dbClt->bulkRemoveOne(bulk, req->query, req->opts);
                    break;
                case mongo_client_req_type::deleteMany:
                    rc = dbClt->bulkRemoveMany(bulk, req->query, req->opts);
                    break;
                default:
                    rc = RESP_RC_TYPE_INVALID;
                    break;
            }
            if (rc) break;
        }
        if (!rc) {
            rc = dbClt->bulkExecute(bulk, resp->reply);
        }
        dbClt->destroyBulk(bulk);
        return rc;
    }

    void handleRequest(mongo_client* dbClt, std::unique_ptr<mongo_client_req> req) {
        auto resp = std::make_unique<mongo_client_resp>(req->req_id);
        switch (req->type) {
            case mongo_client_req_type::dropDatabase:
                resp->rc = dbClt->dropDatabase();
                break;
            case mongo_client_req_type::dropCollection:
                resp->rc = dbClt->dropCollection(req->collName.c_str());
                break;
            case mongo_client_req_type::insertOne:
                resp->rc = dbClt->insertOne(req->collName.c_str(), req->write, req->opts, resp->reply);
                break;
            case mongo_client_req_type::insertMany:
                resp->rc = dbClt->insertMany(req->collName.c_str(), (const bson_t**)req->mult_write, req->mwrite_size, req->opts, resp->reply);
                break;
            case mongo_client_req_type::updateOne:
                resp->rc = dbClt->updateOne(req->collName.c_str(), req->query, req->write, req->opts, resp->reply);
                break;
            case mongo_client_req_type::updateMany:
                resp->rc = dbClt->updateMany(req->collName.c_str(), req->query, req->write, req->opts, resp->reply);
                break;
            case mongo_client_req_type::replaceOne:
                resp->rc = dbClt->replaceOne(req->collName.c_str(), req->query, req->write, req->opts, resp->reply);
                break;
            case mongo_client_req_type::deleteOne:
                resp->rc = dbClt->deleteOne(req->collName.c_str(), req->query, req->opts, resp->reply);
                break;
            case mongo_client_req_type::deleteMany:
                resp->rc = dbClt->deleteMany(req->collName.c_str(), req->query, req->opts, resp->reply);
                break;
            case mongo_client_req_type::find:
                resp->rc = dbClt->find(req->collName.c_str(), req->query, resp->result_docs, req->opts, req->read_prefs);
                break;
            case mongo_client_req_type::command:
                resp->rc = dbClt->command(req->write, req->read_prefs, req->opts, resp->reply);
                break;
            case mongo_client_req_type::bulkExecute:
                resp->rc = bulkExecute(dbClt, req.get(), resp.get());
                break;
            case mongo_client_req_type::execTransaction:
                resp->rc = dbClt->execTransaction(mongo_client_pool::transaction_proc, (void*) req.get(), nullptr, nullptr, resp->reply);
                break;
            default:
                LOG_ERR_MSG("request type invalid, req=%s", req->desc().c_str());
                resp->rc = RESP_RC_TYPE_INVALID;
                break;
        }
        _listener->onResponse(req.get(), resp.get());
    }

    void retryFailure(std::unique_ptr<mongo_client_req> req) {
        LOG_ERR_MSG("request fail, no available client, req=%s", req->desc().c_str());
        auto resp = std::make_unique<mongo_client_resp>(req->req_id);
        resp->rc = RESP_RC_NO_AVAIL_CLT;
        _listener->onResponse(req.get(), resp.get());
    }

    void worker(short thdId) {
        LOG_MSG(LogLevel::Debug, "%s worker thread(%d) start", desc().c_str(), thdId);
        mongoc_client_t* rawClt = nullptr;
        mongo_client* dbClt = nullptr;

        for (;;) {
            std::unique_ptr<mongo_client_req> req;
            int rc = _queue.pop_timeout(req, 100);
            bool handled = false;
            if (rc == zbf::SQ_POP_SUCCESS) {
                rawClt = mongoc_client_pool_try_pop(_pool);
                if (rawClt) { // assume rawClt is connected & safe
                    dbClt = getIdleClient();
                    if (dbClt) {
                        if (0 == dbClt->attach(rawClt, _connParam.dbname.c_str())) {
                            handleRequest(dbClt, std::move(req));
                            handled = true;
                            dbClt->detach();
                        } else {
                            LOG_ERR_MSG("attach db fail, db=%s, req=%s", _connParam.dbname.c_str(), req->desc().c_str());
                        }
                        markClientIdle(dbClt);
                    }
                    mongoc_client_pool_push(_pool, rawClt);
                }

                if (!handled) {
                    if (req->retry_times >= MaxRetryTimes) {
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
                break;;
            }
        }
        LOG_MSG(LogLevel::Debug, "%s worker thread(%d) exit", desc().c_str(), thdId);
    }
    
    mongo_client* getIdleClient() {
        std::lock_guard<std::mutex> lock(_lockClients);
        mongo_client* dbClt = nullptr;
        auto it = _clt2idle.begin();
        for (; it != _clt2idle.end(); ++it) {
            if (it->second) {
                it->second = false;
                dbClt = it->first;
                break;
            }
        }
        if (!dbClt && _clt2idle.size() < _poolSize) {
            dbClt = new mongo_client(_cltSeq.fetch_add(1, std::memory_order_relaxed));
            _clt2idle.insert(std::make_pair(dbClt, false));
        }
        return dbClt;
    }

    void markClientIdle(mongo_client* dbClt) {
        std::lock_guard<std::mutex> lock(_lockClients);
        auto it = _clt2idle.find(dbClt);
        if (it != _clt2idle.end()) {
            it->second = true;
        }
    }

public:
    enum { PoolSize = 10, ThdIdBase = 500, MaxPendingReq = 256, MinReqId = 100, PoolIsBusy = 1, MaxRetryTimes = 3 };
    enum { RESP_RC_PARAM_INVALID = 1001, RESP_RC_TYPE_INVALID = 1002, RESP_RC_NO_AVAIL_CLT = 1003, RESP_RC_NOT_SUPPORT = 1004 };

private:
    mongo_client_connect_param  _connParam;
    mongo_client_pool_listener* _listener; // ref
    int _poolSize;
    mongoc_client_pool_t* _pool;
    std::vector<std::thread *> _workers;
    std::unordered_map<mongo_client*, bool> _clt2idle; // second, idle or not
    std::mutex _lockClients;
    std::atomic<int> _cltSeq;
    std::atomic<mongo_request_id_t> _reqId;
    zbf::SafeQueue<std::unique_ptr<mongo_client_req>, MaxPendingReq> _queue;
};


}

#endif
