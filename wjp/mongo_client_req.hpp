#pragma once

#include <bson/bson.h>
#include <memory>
#include <mongoc/mongoc.h>
#include <vector>
#include "zbf/mem_alloc.hpp"
#include "fszm/serialize_helper.hpp"

namespace wjp {

using fszm::serialize_helper;
typedef uint64_t mongo_request_id_t;

enum mongo_client_req_type {
    dropDatabase,
    dropCollection,
    insertOne,
    insertMany,
    updateOne,
    updateMany,
    replaceOne,
    deleteOne,
    deleteMany,
    find,
    command,
    bulkExecute,
    execTransaction,
    mongoReqNone
};

struct bson_ptr {
    bson_t* p{nullptr};

    bson_ptr() = default;
    explicit bson_ptr(bson_t* bp) : p(bp) {}
    ~bson_ptr() { if (p) bson_destroy(p); }

    bson_ptr(const bson_ptr&) = delete;
    bson_ptr& operator=(const bson_ptr&) = delete;

    bson_ptr(bson_ptr&& o) noexcept : p(o.p) { o.p = nullptr; }
    bson_ptr& operator=(bson_ptr&& o) noexcept {
        if (this != &o) {
            if (p) bson_destroy(p);
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }

    bson_t* get() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

class mongo_serialize_helper : public serialize_helper {
public:
    using serialize_helper::serialize_helper;

    void serialize_bson(const bson_t* val) {
        bool exist = (val != nullptr);
        serialize_bool(exist);
        if (exist) {
            std::string bin((const char*) bson_get_data(val), val->len);
            serialize_str(bin);
        }
    }

    bson_t* deserialize_bson() {
        bson_t* val = nullptr;
        bool exist = deserialize_bool();
        if (exist) {
            std::string bin = deserialize_str();
            val = bson_new_from_data((const uint8_t*) bin.data(), bin.size());
        }
        return val;
    }
};

struct mongo_client_req : zbf::object_tracker<mongo_client_req> {
    // params start
    uint16_t               version{0};
    mongo_client_req_type  type;
    uint8_t                db_slot{0};
    std::string            collName;
    bson_t*                opts;        // find / update / replace / delete / command / insert
    bson_t*                query;       // find / update / replace / delete
    bson_t*                write;       // update / replace / insertOne / command
    bson_t**               mult_write;  // insertMany
    size_t                 mwrite_size; // insertMany
    mongoc_read_prefs_t*   read_prefs;  // find / command, unserializable!!
    std::unique_ptr<mongo_client_req> next; // bulkExecute / execTransaction
    // params end
    mongo_request_id_t     req_id;
    int                    retry_times;
    
    mongo_client_req(mongo_client_req_type type, uint8_t db_slot) : type(type), db_slot(db_slot), 
        opts(nullptr), query(nullptr), write(nullptr), mult_write(nullptr), mwrite_size(0), read_prefs(nullptr), next(nullptr), 
        req_id(0), retry_times(0) {
    }

    mongo_client_req(const mongo_client_req&) = delete;
    mongo_client_req& operator=(const mongo_client_req&) = delete;

    ~mongo_client_req() {
        if (opts)  bson_destroy(opts);
        if (query) bson_destroy(query);
        if (write) bson_destroy(write);
        if (mult_write) {
            for (size_t i = 0; i < mwrite_size; ++i) {
                bson_destroy(mult_write[i]);
            }
            ZBF_FREE(mult_write);
        }
        if (read_prefs) mongoc_read_prefs_destroy(read_prefs);
        // Iteratively destroy linked list to avoid deep recursion
        while (next) {
            auto current = std::move(next);
            next = std::move(current->next);
        }
    }

    static std::unique_ptr<mongo_client_req> newDropDB(uint8_t db_slot) {
        mongo_client_req* req = new mongo_client_req(mongo_client_req_type::dropDatabase, db_slot);
        return std::unique_ptr<mongo_client_req>(req);
    }

    static std::unique_ptr<mongo_client_req> newDropColl(uint8_t db_slot, const char* collName) {
        mongo_client_req* req = new mongo_client_req(mongo_client_req_type::dropCollection, db_slot);
        req->collName = collName;
        return std::unique_ptr<mongo_client_req>(req);
    }

    static std::unique_ptr<mongo_client_req> newInsertOne(uint8_t db_slot, const char* collName, bson_t* doc, bson_t* opts = nullptr) {
        mongo_client_req* req = new mongo_client_req(mongo_client_req_type::insertOne, db_slot);
        req->collName = collName;
        req->write = doc;
        req->opts = opts;
        return std::unique_ptr<mongo_client_req>(req);
    }

    static std::unique_ptr<mongo_client_req> newInsertMany(uint8_t db_slot, const char* collName, bson_t** docs, size_t doc_cnt, bson_t* opts = nullptr) {
        mongo_client_req* req = new mongo_client_req(mongo_client_req_type::insertMany, db_slot);
        req->collName = collName;
        req->mult_write = docs;
        req->mwrite_size = doc_cnt;
        req->opts = opts;
        return std::unique_ptr<mongo_client_req>(req);
    }

    static std::unique_ptr<mongo_client_req> newFind(uint8_t db_slot, const char* collName, bson_t* query, bson_t* opts = nullptr, mongoc_read_prefs_t* read_prefs = nullptr) {
        mongo_client_req* req = new mongo_client_req(mongo_client_req_type::find, db_slot);
        req->collName = collName;
        req->query = query;
        req->opts = opts;
        req->read_prefs = read_prefs;
        return std::unique_ptr<mongo_client_req>(req);
    }

    static std::unique_ptr<mongo_client_req> newUpdate(uint8_t db_slot, const char* collName, bson_t* query, bson_t* write, mongo_client_req_type type, bson_t* opts = nullptr) {
        mongo_client_req* req = new mongo_client_req(type, db_slot); // updateOne, updateMany, replaceOne
        req->collName = collName;
        req->query = query;
        req->write = write;
        req->opts = opts;
        return std::unique_ptr<mongo_client_req>(req);
    }

    static std::unique_ptr<mongo_client_req> newDelete(uint8_t db_slot, const char* collName, bson_t* query, mongo_client_req_type type, bson_t* opts = nullptr) {
        mongo_client_req* req = new mongo_client_req(type, db_slot); // deleteOne, deleteMany
        req->collName = collName;
        req->query = query;
        req->opts = opts;
        return std::unique_ptr<mongo_client_req>(req);
    }

    static std::unique_ptr<mongo_client_req> newCommand(uint8_t db_slot, bson_t* cmd, mongoc_read_prefs_t* read_prefs = nullptr, bson_t* opts = nullptr) {
        mongo_client_req* req = new mongo_client_req(mongo_client_req_type::command, db_slot);
        req->write = cmd;
        req->read_prefs = read_prefs;
        req->opts = opts;
        return std::unique_ptr<mongo_client_req>(req);
    }

    static std::unique_ptr<mongo_client_req> newBulk(uint8_t db_slot, const char* collName, bson_t* opts = nullptr) {
        mongo_client_req* head = new mongo_client_req(mongo_client_req_type::bulkExecute, db_slot);
        head->collName = collName;
        head->opts = opts;
        return std::unique_ptr<mongo_client_req>(head);
    }
    
    static std::unique_ptr<mongo_client_req> newTransaction(uint8_t db_slot) {
        mongo_client_req* head = new mongo_client_req(mongo_client_req_type::execTransaction, db_slot);
        return std::unique_ptr<mongo_client_req>(head);
    }

    static void append(mongo_client_req* head, std::unique_ptr<mongo_client_req> req) {
        mongo_client_req* tail = head;
        while (tail->next != nullptr) {
            tail = tail->next.get();
        }
        tail->next = std::move(req);
    }

    void serialize_one(mongo_serialize_helper& helper) const {
        helper.serialize_uint16(version);
        helper.serialize_int8(type);
        helper.serialize_uint8(db_slot);
        helper.serialize_str(collName);
        helper.serialize_bson(opts);
        helper.serialize_bson(query);
        helper.serialize_bson(write);
        helper.serialize_uint32(mwrite_size);
        for (size_t i = 0; i < mwrite_size; ++i) {
            helper.serialize_bson(mult_write[i]);
        }
    }

    static std::unique_ptr<mongo_client_req> deserialize_one(mongo_serialize_helper& helper) {
        mongo_client_req* req = new mongo_client_req(mongo_client_req_type::mongoReqNone, 0);
        req->version = helper.deserialize_uint16();
        req->type = (mongo_client_req_type) helper.deserialize_int8();
        req->db_slot     = helper.deserialize_uint8();
        req->collName    = helper.deserialize_str();
        req->opts        = helper.deserialize_bson();
        req->query       = helper.deserialize_bson();
        req->write       = helper.deserialize_bson();
        req->mwrite_size = helper.deserialize_uint32();
        req->mult_write  = (bson_t**) ZBF_MALLOC(sizeof(bson_t*) * req->mwrite_size);
        for (size_t i = 0; i < req->mwrite_size; ++i) {
            req->mult_write[i] = helper.deserialize_bson();
        }
        return std::unique_ptr<mongo_client_req>(req);
    }

    std::string serialize() const {
        mongo_serialize_helper helper;
        const mongo_client_req* head = this;
        while (head != nullptr) {
            head->serialize_one(helper);
            head = head->next.get();
            helper.serialize_bool(head != nullptr);
        }
        return helper.data();
    }

    static std::unique_ptr<mongo_client_req> deserialize(const std::string& data) {
        mongo_serialize_helper helper(data);
        auto head = deserialize_one(helper);
        auto* current = head.get();
        bool next = helper.deserialize_bool();
        while (next) {
            current->next = deserialize_one(helper);
            current = current->next.get();
            next = helper.deserialize_bool();
        }
        return head;
    }

    std::string desc() const {
        std::string str = std::string("{ver=")  + std::to_string(version);
        str += std::string(",db=")   + std::to_string(db_slot);
        str += std::string(",type=") + std::to_string((int)type);
        str += std::string(",coll=") + collName;
        str += std::string(",req=")  + std::to_string(req_id) + std::string("}");
        return str;
    }
};


struct mongo_client_resp : zbf::object_tracker<mongo_client_resp> {
    uint16_t                  version{0};
    mongo_request_id_t        req_id;
    int                       rc;
    bson_t*                   reply;       // update / replace / delete / command / insert
    std::vector<bson_ptr>     result_docs; // find

    mongo_client_resp(mongo_request_id_t id) : req_id(id), rc(-1), reply(nullptr) {
    }

    mongo_client_resp(const mongo_client_resp&) = delete;
    mongo_client_resp& operator=(const mongo_client_resp&) = delete;

    ~mongo_client_resp() {
        if (reply) {
            bson_destroy(reply);
        }
        // result_docs is auto-destructed; bson_ptr's destructor handles bson_destroy
    }

    std::string serialize() const {
        mongo_serialize_helper helper;
        helper.serialize_uint16(version);
        helper.serialize_int64(req_id);
        helper.serialize_int32(rc);
        helper.serialize_bson(reply);
        helper.serialize_uint32(result_docs.size());
        for (const auto& doc : result_docs) {
            helper.serialize_bson(doc.get());
        }
        return helper.data();
    }

    static std::unique_ptr<mongo_client_resp> deserialize(const std::string& data) {
        mongo_serialize_helper helper(data);
        mongo_client_resp* resp = new mongo_client_resp(0);
        resp->version = helper.deserialize_uint16();
        resp->req_id = helper.deserialize_int64();
        resp->rc     = helper.deserialize_int32();
        resp->reply  = helper.deserialize_bson();
        size_t size  = helper.deserialize_uint32();
        for (size_t i = 0; i < size; ++i) {
            bson_t* bson_obj = helper.deserialize_bson();
            resp->result_docs.emplace_back(bson_obj);
        }
        return std::unique_ptr<mongo_client_resp>(resp);
    }
};


}
