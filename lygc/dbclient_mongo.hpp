#pragma once

#include <functional>
#include "net_server.hpp"
#include "wjp/mongo_client_req.hpp"

namespace lygc {

using wjp::mongo_client_req;
using wjp::mongo_client_resp;

class DBClientMongo {
public:
    using Handler = std::function<void(const lymsg_header*, const mongo_client_resp*)>;

    DBClientMongo(NetClient* clt) : _clt(clt) {}

    int request(lymsg_header* h, const mongo_client_req* req, Handler cb) {
        auto raw = [cb](const lymsg_header* rh, const std::string& rd) {
            auto resp = mongo_client_resp::deserialize(rd);
            cb(rh, resp.get());
        };
        h->type = LYMSG_TYPE_DB_MONGO;
        return _clt->request(h, req->serialize(), raw);
    }

private:
    NetClient* _clt;
};

}  // namespace lygc
