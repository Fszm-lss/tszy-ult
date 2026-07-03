#pragma once

#include <functional>
#include "net_server.hpp"
#include "wjp/mysql_client_req.hpp"

namespace lygc {

using wjp::mysql_client_req;
using wjp::mysql_client_resp;

class DBClientMySQL {
public:
    using Handler = std::function<void(const lymsg_header*, const mysql_client_resp*)>;

    DBClientMySQL(NetClient* clt) : _clt(clt) {}

    int request(lymsg_header* h, const mysql_client_req* req, Handler cb) {
        auto raw = [cb](const lymsg_header* rh, const std::string& rd) {
            auto resp = mysql_client_resp::deserialize(rd);
            cb(rh, resp.get());
        };
        h->type = LYMSG_TYPE_DB_MYSQL;
        return _clt->request(h, req->serialize(), raw);
    }

private:
    NetClient* _clt;
};

}  // namespace lygc
