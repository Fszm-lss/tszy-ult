#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <hiredis/hiredis.h>
#include "zbf/log_utils.hpp"
#include "zbf/safequeue.hpp"
#include "redis_client_req.hpp"


// link with -lhiredis
namespace wjp {

using zbf::LogLevel;

struct redis_client_connect_param {
    std::string host;
    int         port;
    std::string auth_str;
    std::string dbname;
    int         timeout;
    uint8_t     DBSlotNo{0};

    redis_client_connect_param(const std::string& host, const std::string& auth_str, const std::string& dbname, int port = 6379, int timeout = 1000) {
        this->host     = host;
        this->auth_str = auth_str;
        this->dbname   = dbname;
        this->port     = port;
        this->timeout  = timeout;
        this->DBSlotNo = 0;
    }
};

// thread unsafe
class redis_client {
public:    
    redis_client() : _context(nullptr), _connState(false), _param("127.0.0.1", "", "") {
    }

    ~redis_client() {
        close();
    }

public:
    int open(const std::string& host, int port, const std::string& auth_str, const std::string& dbname, int timeout = 1000, int* err = 0, std::string* errmsg = 0) {        
        _param = redis_client_connect_param(host, auth_str, dbname, port, timeout);
        return connect(err, errmsg);
    }

    int open(const redis_client_connect_param& param, int* err = 0, std::string* errmsg = 0) {
        _param = param;
        return connect(err, errmsg);
    }

    void close() {
        if (_context) {
            redisFree(_context);
            _context = nullptr;
        }
        flushConnState(false);
    }

    int setTimeout(int timeout) {
        if (!_context) return -1;
        struct timeval tv;
        tv.tv_sec = timeout / 1000;
        tv.tv_usec = (timeout % 1000) * 1000;
        return redisSetTimeout(_context, tv);
    }

    // Binary-safe command execution interface (based on redisCommandArgv)
    redisReply* execCommand(const std::vector<std::string>& args, int* err = 0, std::string* errmsg = 0) {
        if (!_context) {
            if (connect(err, errmsg)) {
                return nullptr;
            }
        }

        std::vector<const char*> argv(args.size());
        std::vector<size_t> argvlen(args.size());
        for (size_t i = 0; i < args.size(); ++i) {
            argv[i] = args[i].c_str();
            argvlen[i] = args[i].size();
        }

        redisReply* reply = (redisReply*) redisCommandArgv(_context, args.size(), argv.data(), argvlen.data());
        if (!reply) {
            if (connect(err, errmsg)) {
                return nullptr;
            }
            reply = (redisReply*) redisCommandArgv(_context, args.size(), argv.data(), argvlen.data());
        }

        // Connection-level error: !reply or _context->err
        if (!reply || _context->err) {
            if (err)    *err = _context->err ? _context->err : REDIS_REPLY_ERROR;
            if (errmsg) *errmsg = _context->errstr ? std::string(_context->errstr) : "unknown error";
            if (reply) freeReplyObject(reply);
            close();
            return nullptr;
        }

        // Protocol-level error: reply->type == REDIS_REPLY_ERROR, connection still usable
        if (reply->type == REDIS_REPLY_ERROR) {
            if (err)    *err = REDIS_REPLY_ERROR;
            if (errmsg) *errmsg = reply->str ? std::string(reply->str) : "unknown error";
            freeReplyObject(reply);
            // Do not close the connection!
            return nullptr;
        }

        flushConnState(true);
        return reply;
    }

    void freeReply(redisReply** reply) {
        if (reply && *reply) {
            freeReplyObject(*reply);
            *reply = nullptr;
        }
    }

    enum { ExecSuccess = 0, ExecKeyNotExist = 10001, ExecUnexpectedType = 10002 };

    bool execCommandStat(const std::vector<std::string>& args, int* ec = nullptr) {
        bool ok = false;
        int err = 0;
        std::string errmsg;
        redisReply* reply = execCommand(args, &err, &errmsg);
        
        if (reply) {
            if (reply->type == REDIS_REPLY_STATUS) {
                ok = (reply->str && reply->len >= 2 && strncmp(reply->str, "OK", 2) == 0);
                if (ec) *ec = ExecSuccess;
            } else if (reply->type == REDIS_REPLY_NIL) {
                if (ec) *ec = ExecKeyNotExist;
            } else {
                if (ec) *ec = ExecUnexpectedType;
            }
            freeReply(&reply);
        } else {
            if (ec) *ec = err;
            LOG_ERR_MSG("err=%d, errmsg=%s", err, errmsg.c_str());
        }
        return ok;
    }

    int execCommandInt(const std::vector<std::string>& args, int* ec = nullptr) {
        int err = 0;
        std::string errmsg;
        redisReply* reply = execCommand(args, &err, &errmsg);
        int rc = 0;
        if (reply) {
            if (reply->type == REDIS_REPLY_INTEGER) {
                rc = reply->integer;
                if (ec) *ec = ExecSuccess;
            } else if (reply->type == REDIS_REPLY_NIL) {
                if (ec) *ec = ExecKeyNotExist;
            } else {
                if (ec) *ec = ExecUnexpectedType;
            }
            freeReply(&reply);
        } else {
            if (ec) *ec = err;
            LOG_ERR_MSG("err=%d, errmsg=%s", err, errmsg.c_str());
        }
        return rc;
    }    

    std::string execCommandStr(const std::vector<std::string>& args, int* ec = nullptr) {
        int err = 0;
        std::string errmsg;
        redisReply* reply = execCommand(args, &err, &errmsg);
        std::string result;
        if (reply) {
            if (reply->type == REDIS_REPLY_STRING) {
                result = getReplyStr(reply);
                if (ec) *ec = ExecSuccess;
            } else if (reply->type == REDIS_REPLY_NIL) {
                if (ec) *ec = ExecKeyNotExist;
            } else {
                if (ec) *ec = ExecUnexpectedType;
            }
            freeReply(&reply);
        } else {
            if (ec) *ec = err;
            LOG_ERR_MSG("err=%d, errmsg=%s", err, errmsg.c_str());
        }
        return result;
    }

    StringArray execCommandSA(const std::vector<std::string>& args, int* ec = nullptr) {
        int err = 0;
        std::string errmsg;
        redisReply* reply = execCommand(args, &err, &errmsg);
        StringArray result;
        if (reply) {
            if (reply->type == REDIS_REPLY_ARRAY) {
                for (size_t i = 0; i < reply->elements; ++i) {
                    redisReply* r = reply->element[i];
                    if (r->type == REDIS_REPLY_STRING) {
                        result.push_back(getReplyStr(r));
                    } else if (r->type == REDIS_REPLY_INTEGER) {
                        result.push_back(std::to_string(r->integer));
                    } else if (r->type == REDIS_REPLY_NIL) {
                        // ignore
                    }
                }
                if (ec) *ec = ExecSuccess;
            } else if (reply->type == REDIS_REPLY_NIL) {
                if (ec) *ec = ExecKeyNotExist;
            } else {
                if (ec) *ec = ExecUnexpectedType;
            }
            freeReply(&reply);
        } else {
            if (ec) *ec = err;
            LOG_ERR_MSG("err=%d, errmsg=%s", err, errmsg.c_str());
        }
        return result;
    }

    StringMap execCommandSM(const std::vector<std::string>& args, int* ec = nullptr) {
        int err = 0;
        std::string errmsg;
        redisReply* reply = execCommand(args, &err, &errmsg);
        StringMap result;
        if (reply) {
            if (reply->type == REDIS_REPLY_ARRAY) {
                std::string key;
                for (size_t i = 0; i < reply->elements; ++i) {
                    redisReply* r = reply->element[i];
                    if (r->type == REDIS_REPLY_STRING) {
                        if (i % 2 == 0) {
                            key = getReplyStr(r);
                        } else {
                            result[key] = getReplyStr(r);
                        }
                    } else if (r->type == REDIS_REPLY_INTEGER) {
                        if (i % 2 == 0) {
                            key = std::to_string(r->integer);
                        } else {
                            result[key] = std::to_string(r->integer);
                        }
                    } else if (r->type == REDIS_REPLY_NIL) {
                        if (i % 2 == 0) {
                            key = std::string();
                        } else {
                            result[key] = std::string();
                        }
                    }
                }
                if (ec) *ec = ExecSuccess;
            } else if (reply->type == REDIS_REPLY_NIL) {
                if (ec) *ec = ExecKeyNotExist;
            } else {
                if (ec) *ec = ExecUnexpectedType;
            }
            freeReply(&reply);
        } else {
            if (ec) *ec = err;
            LOG_ERR_MSG("err=%d, errmsg=%s", err, errmsg.c_str());
        }
        return result;
    }

    ScoreArray execCommandSCA(const std::vector<std::string>& args, int* ec = nullptr) {
        std::vector<std::string> actualArgs = args;
        actualArgs.push_back("WITHSCORES");
        int err = 0;
        std::string errmsg;
        redisReply* reply = execCommand(actualArgs, &err, &errmsg);
        ScoreArray result;
        if (reply) {
            if (reply->type == REDIS_REPLY_ARRAY) {
                double score = 0;
                std::string str;
                for (size_t i = 0; i < reply->elements; ++i) {
                    redisReply* r = reply->element[i];
                    if (r->type == REDIS_REPLY_STRING) {
                        if (i % 2 == 0) str = getReplyStr(r);
                        else {
                            score = strtod(getReplyStr(r).c_str(), nullptr);
                            result.push_back(std::pair<double, std::string>(score, str));
                        }
                    } else if (r->type == REDIS_REPLY_INTEGER) {
                        if (i % 2 == 0) str = std::to_string(r->integer);
                        else {
                            score = (double) r->integer;
                            result.push_back(std::pair<double, std::string>(score, str));
                        }
                    } else if (r->type == REDIS_REPLY_NIL) {
                        if (i % 2 == 0) str = std::string();
                        else {
                            score = 0;
                            if (!str.empty()) result.push_back(std::pair<double, std::string>(score, str));
                        }
                    }
                }
                if (ec) *ec = ExecSuccess;
            } else if (reply->type == REDIS_REPLY_NIL) {
                if (ec) *ec = ExecKeyNotExist;
            } else {
                if (ec) *ec = ExecUnexpectedType;
            }
            freeReply(&reply);
        } else {
            if (ec) *ec = err;
            LOG_ERR_MSG("err=%d, errmsg=%s", err, errmsg.c_str());
        }
        return result;
    }

public:
    // KEY
    // return deleted count
    int DEL(const std::string& key, int* ec = nullptr) {
        return execCommandInt({"DEL", key}, ec);
    }

    bool EXISTS(const std::string& key, int* ec = nullptr) {
        return 1 == execCommandInt({"EXISTS", key}, ec);
    }

    bool EXPIRE(const std::string& key, int seconds, int* ec = nullptr) {
        return 1 == execCommandInt({"EXPIRE", key, std::to_string(seconds)}, ec);
    }

    bool PERSIST(const std::string& key, int* ec = nullptr) {
        return 1 == execCommandInt({"PERSIST", key}, ec);
    }

    int TTL(const std::string& key, int* ec = nullptr) {
        return execCommandInt({"TTL", key}, ec);
    }

    // STRING
    bool SET(const std::string& key, const std::string& val, int* ec = nullptr) {
        return execCommandStat({"SET", key, val}, ec);
    }

    // set if not exist, do nothing if exist
    // 1 : set | 0 : not set
    int SETNX(const std::string& key, const std::string& val, int* ec = nullptr) {
        return execCommandInt({"SETNX", key, val}, ec);
    }

    std::string GET(const std::string& key, int* ec = nullptr) {
        return execCommandStr({"GET", key}, ec);
    }

    bool MSET(const StringMap& data, int* ec = nullptr) {
        std::vector<std::string> args = {"MSET"};
        for (const auto& it : data) {
            args.push_back(it.first);
            args.push_back(it.second);
        }
        return execCommandStat(args, ec);
    }

    StringArray MGET(const StringArray& keys, int* ec = nullptr) {
        std::vector<std::string> args = {"MGET"};
        args.insert(args.end(), keys.begin(), keys.end());
        return execCommandSA(args, ec);
    }

    // HASH
    // return deleted count
    int HDEL(const std::string& key, const std::string& field, int* ec = nullptr) {
        return execCommandInt({"HDEL", key, field}, ec);
    }
    int HDEL(const std::string& key, const StringArray& fields, int* ec = nullptr) {
        std::vector<std::string> args = {"HDEL", key};
        args.insert(args.end(), fields.begin(), fields.end());
        return execCommandInt(args, ec);
    }

    bool HEXISTS(const std::string& key, const std::string& field, int* ec = nullptr) {
        return 1 == execCommandInt({"HEXISTS", key, field}, ec);
    }

    int HLEN(const std::string& key, int* ec = nullptr) {
        return execCommandInt({"HLEN", key}, ec);
    }

    // return 1 if set an new field, 0 if set an old field
    int HSET(const std::string& key, const std::string& field, const std::string& val, int* ec = nullptr) {
        return execCommandInt({"HSET", key, field, val}, ec);
    }

    std::string HGET(const std::string& key, const std::string& field, int* ec = nullptr) {
        return execCommandStr({"HGET", key, field}, ec);
    }

    bool HMSET(const std::string& key, const StringMap& data, int* ec = nullptr) {
        std::vector<std::string> args = {"HMSET", key};
        for (const auto& it : data) {
            args.push_back(it.first);
            args.push_back(it.second);
        }
        return execCommandStat(args, ec);
    }

    StringArray HMGET(const std::string& key, const StringArray& fields, int* ec = nullptr) {
        std::vector<std::string> args = {"HMGET", key};
        args.insert(args.end(), fields.begin(), fields.end());
        return execCommandSA(args, ec);
    }

    StringMap HGETALL(const std::string& key, int* ec = nullptr) {
        return execCommandSM({"HGETALL", key}, ec);
    }

    // LIST
    std::string LINDEX(const std::string& key, int index, int* ec = nullptr) {
        return execCommandStr({"LINDEX", key, std::to_string(index)}, ec);
    }

    int LLEN(const std::string& key, int* ec = nullptr) {
        return execCommandInt({"LLEN", key}, ec);
    }

    StringArray LRANGE(const std::string& key, int start, int stop, int* ec = nullptr) {
        return execCommandSA({"LRANGE", key, std::to_string(start), std::to_string(stop)}, ec);
    }

    bool LTRIM(const std::string& key, int start, int stop, int* ec = nullptr) {
        return execCommandStat({"LTRIM", key, std::to_string(start), std::to_string(stop)}, ec);
    }

    std::string LPOP(const std::string& key, int* ec = nullptr) {
        return execCommandStr({"LPOP", key}, ec);
    }

    // return the current size of list
    int LPUSH(const std::string& key, const std::string& val, int* ec = nullptr) {
        return execCommandInt({"LPUSH", key, val}, ec);
    }
    int LPUSH(const std::string& key, const StringArray& vals, int* ec = nullptr) {
        std::vector<std::string> args = {"LPUSH", key};
        args.insert(args.end(), vals.begin(), vals.end());
        return execCommandInt(args, ec);
    }

    std::string RPOP(const std::string& key, int* ec = nullptr) {
        return execCommandStr({"RPOP", key}, ec);
    }

    int RPUSH(const std::string& key, const std::string& val, int* ec = nullptr) {
        return execCommandInt({"RPUSH", key, val}, ec);
    }
    int RPUSH(const std::string& key, const StringArray& vals, int* ec = nullptr) {
        std::vector<std::string> args = {"RPUSH", key};
        args.insert(args.end(), vals.begin(), vals.end());
        return execCommandInt(args, ec);
    }

    // SET
    // return succeed add count
    int SADD(const std::string& key, const std::string& member, int* ec = nullptr) {
        return execCommandInt({"SADD", key, member}, ec);
    }
    int SADD(const std::string& key, const StringArray& members, int* ec = nullptr) {
        std::vector<std::string> args = {"SADD", key};
        args.insert(args.end(), members.begin(), members.end());
        return execCommandInt(args, ec);
    }

    // return total count
    int SCARD(const std::string& key, int* ec = nullptr) {
        return execCommandInt({"SCARD", key}, ec);
    }

    bool SISMEMBER(const std::string& key, const std::string& member, int* ec = nullptr) {
        return 1 == execCommandInt({"SISMEMBER", key, member}, ec);
    }

    std::set<std::string> SMEMBERS(const std::string& key, int* ec = nullptr) {
        StringArray temp = execCommandSA({"SMEMBERS", key}, ec);
        std::set<std::string> result(temp.begin(), temp.end());
        return result;
    }

    std::string SPOP(const std::string& key, int* ec = nullptr) {
        return execCommandStr({"SPOP", key}, ec);
    }

    // return deleted count
    int SREM(const std::string& key, const std::string& member, int* ec = nullptr) {
        return execCommandInt({"SREM", key, member}, ec);
    }
    int SREM(const std::string& key, const StringArray& members, int* ec = nullptr) {
        std::vector<std::string> args = {"SREM", key};
        args.insert(args.end(), members.begin(), members.end());
        return execCommandInt(args, ec);
    }

    // SCORE SET
    // return succeed add count
    int ZADD(const std::string& key, double score, const std::string& member, int* ec = nullptr) {
        return execCommandInt({"ZADD", key, std::to_string(score), member}, ec);
    }
    int ZADD(const std::string& key, const ScoreArray& members, int* ec = nullptr) {
        std::vector<std::string> args = {"ZADD", key};
        for (const auto& m : members) {
            args.push_back(std::to_string(m.first));
            args.push_back(m.second);
        }
        return execCommandInt(args, ec);
    }

    int ZCARD(const std::string& key, int* ec = nullptr) {
        return execCommandInt({"ZCARD", key}, ec);
    }

    int ZCOUNT(const std::string& key, double min, double max, int* ec = nullptr) {
        return execCommandInt({"ZCOUNT", key, std::to_string(min), std::to_string(max)}, ec);
    }

    ScoreArray ZRANGE(const std::string& key, int start, int stop) {
        return execCommandSCA({"ZRANGE", key, std::to_string(start), std::to_string(stop)});
    }

    ScoreArray ZRANGEBYSCORE(const std::string& key, double min, double max) {
        return execCommandSCA({"ZRANGEBYSCORE", key, std::to_string(min), std::to_string(max)});
    }

    int ZREM(const std::string& key, const std::string& member, int* ec = nullptr) {
        return execCommandInt({"ZREM", key, member}, ec);
    }
    int ZREM(const std::string& key, const StringArray& members, int* ec = nullptr) {
        std::vector<std::string> args = {"ZREM", key};
        args.insert(args.end(), members.begin(), members.end());
        return execCommandInt(args, ec);
    }

    int ZREMRANGEBYSCORE(const std::string& key, double min, double max, int* ec = nullptr) {
        return execCommandInt({"ZREMRANGEBYSCORE", key, std::to_string(min), std::to_string(max)}, ec);
    }

    ScoreArray ZREVRANGE(const std::string& key, int start, int stop) {
        return execCommandSCA({"ZREVRANGE", key, std::to_string(start), std::to_string(stop)});
    }

    ScoreArray ZREVRANGEBYSCORE(const std::string& key, double max, double min) {
        return execCommandSCA({"ZREVRANGEBYSCORE", key, std::to_string(max), std::to_string(min)});
    }

    double ZSCORE(const std::string& key, const std::string& member, int* ec = nullptr) {
        int local_ec = 0;
        std::string str = execCommandStr({"ZSCORE", key, member}, &local_ec);
        if (ec) *ec = local_ec;
        if (local_ec != 0) return std::numeric_limits<double>::quiet_NaN();
        return strtod(str.c_str(), nullptr);
    }

    bool getConnState() {
        return _connState;
    }

private:
    std::string getReplyStr(redisReply* r) {
        return r->str ? std::string(r->str, r->len) : std::string();
    }

    int connect(int* err = 0, std::string* errmsg = 0) {
        close();
        struct timeval tv;
        tv.tv_sec = _param.timeout / 1000;
        tv.tv_usec = (_param.timeout % 1000) * 1000;
        _context = redisConnectWithTimeout(_param.host.c_str(), _param.port, tv);

        // clear error
        if (err)       *err = 0;
        if (errmsg) *errmsg = "";

        if (_context == nullptr) {
            if (err)       *err = REDIS_ERR_OOM;
            if (errmsg) *errmsg = "redisConnectWithTimeout returned null (out of memory)";
            return -1;
        }
        if (_context->err) {
            if (err)       *err = _context->err;
            if (errmsg) *errmsg = _context->errstr ? std::string(_context->errstr) : "unknown error";
            close();
            return -2;
        }
        if (REDIS_ERR == redisSetTimeout(_context, tv)) {
            if (err)    *err = _context->err;
            if (errmsg) *errmsg = _context->errstr ? std::string(_context->errstr) : "redisSetTimeout failed";
            close();
            return -3;
        }

        if (_param.auth_str.empty()) {
            flushConnState(true);
            LOG_MSG(LogLevel::Warn, "skip auth, auth_str is empty");
            return 0;
        }

        // Use redisCommandArgv to execute AUTH, binary-safe
        const char* auth_argv[] = {"AUTH", _param.auth_str.c_str()};
        size_t auth_argvlen[] = {4, _param.auth_str.size()};
        redisReply* reply = (redisReply*) redisCommandArgv(_context, 2, auth_argv, auth_argvlen);

        if (!reply || reply->type == REDIS_REPLY_ERROR || _context->err) {
            if (err)       *err = _context->err ? _context->err : REDIS_REPLY_ERROR;
            if (errmsg) {
                if (_context->err && _context->errstr) {
                    *errmsg = _context->errstr;
                } else if (reply && reply->type == REDIS_REPLY_ERROR && reply->str) {
                    *errmsg = reply->str;
                } else {
                    *errmsg = "unknown error";
                }
            }
            if (reply) freeReplyObject(reply);
            close();
            return -4;
        }

        freeReplyObject(reply);
        flushConnState(true);
        return 0;
    }

    void flushConnState(bool state) {
        _connState = state;
    }

private:
    redisContext*              _context;
    bool                       _connState;
    redis_client_connect_param _param;
};


class redis_client_pool_listener {
public:
    virtual void onResponse(const redis_client_req* req, const redis_client_resp* resp) = 0;
};

class redis_client_pool {
public:
    redis_client_pool(const redis_client_connect_param& param, redis_client_pool_listener* listener)
        : _reqId(MinReqId), _poolSize(0), _connParam(param), _listener(listener) {
        assert(_listener != nullptr);
    }

    virtual ~redis_client_pool() {
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

        std::unique_ptr<redis_client_req> req;
        while (_queue.raw_pop(req)) {}

        uninitPool();
        LOG_MSG(LogLevel::Debug, "%s stop", desc().c_str());
    }

    void stop(bool join = false) {
         _queue.stop();
         if (join) serveUtilStop();
    }

    redis_request_id_t submit(std::unique_ptr<redis_client_req> req) {
        redis_request_id_t rc = genRequestId();
        req->req_id = rc;
        if (_queue.push(std::move(req))) {
            return rc;
        }
        LOG_ERR_MSG("submit fail, queue is full, reqId=%lu", rc);
        return PoolIsBusy;
    }

    const redis_client_connect_param& connectParam() {
        return _connParam;
    }

    std::string desc() {
        return std::string("redis_client_pool-") + _connParam.host + ":" + std::to_string(_connParam.port);
    }

private:
    redis_request_id_t genRequestId() {
        // uint64_t space is extremely large (1.8x10^19), will not overflow in practice
        return _reqId.fetch_add(1, std::memory_order_relaxed);
    }

    void uninitPool() {
        std::unordered_map<redis_client*, bool>::iterator it = _clt2idle.begin();
        for ( ; it != _clt2idle.end(); ++it) {
            redis_client* clt = it->first;
            clt->close();
            delete clt;
        }
        _clt2idle.clear();
    }    

    void handleRequest(redis_client* client, std::unique_ptr<redis_client_req> req) {
        assert(client != nullptr);
        assert(req != nullptr);

        int ec = 0;
        auto resp = std::make_unique<redis_client_resp>();
        resp->req_id = req->req_id;
        resp->type   = req->type;
        switch (req->type) {
            case redis_client_req_type::execCommandStat: {
                resp->resStat = client->execCommandStat(req->argv, &ec);
                resp->rc = (ec != 0) ? -1 : 0;
                _listener->onResponse(req.get(), resp.get());
                break;
            }
            case redis_client_req_type::execCommandInt: {
                resp->resInt = client->execCommandInt(req->argv, &ec);
                resp->rc = (ec != 0) ? -1 : 0;
                _listener->onResponse(req.get(), resp.get());
                break;
            }
            case redis_client_req_type::execCommandStr: {
                resp->resStr = client->execCommandStr(req->argv, &ec);
                resp->rc = (ec != 0) ? -1 : 0;
                _listener->onResponse(req.get(), resp.get());
                break;
            }
            case redis_client_req_type::execCommandSA: {
                resp->resSA = client->execCommandSA(req->argv, &ec);
                resp->rc = (ec != 0) ? -1 : 0;
                _listener->onResponse(req.get(), resp.get());
                break;
            }
            case redis_client_req_type::execCommandSM: {
                resp->resSM = client->execCommandSM(req->argv, &ec);
                resp->rc = (ec != 0) ? -1 : 0;
                _listener->onResponse(req.get(), resp.get());
                break;
            }
            case redis_client_req_type::execCommandSCA: {
                resp->resSCA = client->execCommandSCA(req->argv, &ec);
                resp->rc = (ec != 0) ? -1 : 0;
                _listener->onResponse(req.get(), resp.get());
                break;
            }
            default: {
                LOG_ERR_MSG("invalid request, req=%s", req->desc().c_str());
                resp->rc = RESP_RC_TYPE_INVALID;
                ec = -1;
                _listener->onResponse(req.get(), resp.get());
                break;
            }
        }
        if (ec) {
            LOG_ERR_MSG("handleRequest failed, req=%s, err=%d", req->desc().c_str(), ec);
        }
    }

    void retryFailure(std::unique_ptr<redis_client_req> req) {
        LOG_ERR_MSG("request fail, no available client, req=%s", req->desc().c_str());
        auto resp = std::make_unique<redis_client_resp>();
        resp->req_id = req->req_id;
        resp->type   = req->type;
        resp->rc = RESP_RC_NO_AVAIL_CLT;
        _listener->onResponse(req.get(), resp.get());
    }

    void worker(short thdId) {
        LOG_MSG(LogLevel::Debug, "%s worker thread(%d) start", desc().c_str(), thdId);

        for (;;) {
            std::unique_ptr<redis_client_req> req;
            int rc = _queue.pop_timeout(req, 100);
            if (rc == zbf::SQ_POP_SUCCESS) {
                redis_client* client = getIdleClient();
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

    redis_client* getIdleClient() {
        redis_client* clt = nullptr;
        redis_client* newClt = nullptr;
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
                newClt = new redis_client();
                _clt2idle.insert(std::make_pair(newClt, false));
            }
        }
        if (!clt && newClt) {
            int err = 0;
            std::string errmsg;
            int rc = newClt->open(_connParam, &err, &errmsg);
            std::lock_guard<std::mutex> lock(_lockClients);
            if (0 == rc) {
                clt = newClt;
            } else {
                LOG_ERR_MSG("redis_client open failed, err=%d, errmsg=%s", err, errmsg.c_str());
                _clt2idle.erase(newClt);
                newClt->close();
                delete newClt;
                newClt = nullptr;
            }
        }
        return clt;
    }

    void markClientIdle(redis_client* clt) {
        std::lock_guard<std::mutex> lock(_lockClients);
        auto it = _clt2idle.find(clt);
        if (it != _clt2idle.end()) {
            it->second = true;
        }
    }

    void closeClient(redis_client* clt) {
        {
            std::lock_guard<std::mutex> lock(_lockClients);
            _clt2idle.erase(clt);
        }
        clt->close();
        delete clt;
    }

public:
    enum { PoolSize = 10, ThdIdBase = 700, MaxPendingReq = 256, MinReqId = 100, PoolIsBusy = 1, MaxRetryTimes = 3 };
    enum { RESP_RC_PARAM_INVALID = 1001, RESP_RC_TYPE_INVALID = 1002, RESP_RC_NO_AVAIL_CLT = 1003 };

private:
    std::vector<std::thread *> _workers;
    std::atomic<redis_request_id_t> _reqId;
    zbf::SafeQueue<std::unique_ptr<redis_client_req>, MaxPendingReq> _queue;
    std::unordered_map<redis_client*, bool> _clt2idle;
    std::mutex _lockClients;
    int _poolSize;
    redis_client_connect_param _connParam;
    redis_client_pool_listener* _listener; // ref
};

}
