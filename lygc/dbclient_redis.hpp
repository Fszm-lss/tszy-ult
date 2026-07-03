#pragma once

#include <functional>
#include "net_server.hpp"
#include "wjp/redis_client_req.hpp"

namespace lygc {

using wjp::redis_client_req;
using wjp::redis_client_resp;

class DBClientRedis {
public:
    using Handler = std::function<void(const lymsg_header*, const redis_client_resp*)>;

    DBClientRedis(NetClient* clt) : _clt(clt) {}

    int request(lymsg_header* h, const redis_client_req* req, Handler cb) {
        auto raw = [cb](const lymsg_header* rh, const std::string& rd) {
            auto resp = redis_client_resp::deserialize(rd);
            cb(rh, resp.get());
        };
        h->type = LYMSG_TYPE_DB_REDIS;
        return _clt->request(h, req->serialize(), raw);
    }

private:
    NetClient* _clt;
};

}  // namespace lygc
