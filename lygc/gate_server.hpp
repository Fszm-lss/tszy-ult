#pragma once

#include "net_server.hpp"
#include "handshake_helper_v2.hpp"

namespace lygc {

class GateUser : public NetUser {
public:
    using NetUser::NetUser;

    virtual void onRequest(const lymsg_header* reqHeader, const std::string& reqData) override;

    virtual tcp_message* createResponse(const lymsg_header* reqHeader, const std::string& respData) override;

private:
    void finHandshake(bool success, const std::string& shareKey = std::string()) {
        if (success && shareKey.length() >= 32) {
            _secret  = shareKey.substr(0, 16);
            _ivec    = shareKey.substr(16, 16);
            _enctype = openssl_utils::eEncryptType::eET_AES_CFB;
            _handshakeFin.store(true);
        } else {
            _handshakeFin.store(false);
        }
    }

    std::string decrypt(const std::string& data) {
        std::string result;
        int rc = openssl_utils::symmetric_encrypt(data, _secret, _ivec, _enctype, false, result);
        if (rc) {
            LOG_ERR_MSG("symmetric decrypt fail, rc=%d, size=%d", rc, data.size());
        }
        return result;
    }

    std::string encrypt(const std::string& data) {
        std::string result;
        int rc = openssl_utils::symmetric_encrypt(data, _secret, _ivec, _enctype, true, result);
        if (rc) {
            LOG_ERR_MSG("symmetric encrypt fail, rc=%d, size=%d", rc, data.size());
        }
        return result;
    }

    bool isHandshake() {
        return _handshakeFin.load();
    }

private:
    std::string _secret;
    std::string _ivec;
    openssl_utils::eEncryptType _enctype{openssl_utils::eEncryptType::eET_AES_CFB};
    std::atomic<bool> _handshakeFin{false};
};

class GateServer : public NetServer {
public:
    using NetServer::NetServer;

    virtual tcpsock_user* createUser() override {
        return new GateUser(this, NetServer::origin());
    }

    bool loadPrivateKey(const std::string& keyPath) {
        return _serverSide.loadRSAKey(keyPath, false);
    }

    std::string onHandshake(const std::string& reqData, std::string& shareKey) {
        std::lock_guard<std::mutex> lock(_lockHandshake);
        HandshakeReq* hsReq = _serverSide.decryptHSReq(reqData);
        if (!hsReq) {
            return std::string();
        }
        _serverSide.logHSReq(hsReq);

        HandshakeResp* hsResp = _serverSide.creatHSResp(hsReq);
        if (!hsResp) {
            delete hsReq;
            return std::string();
        }        
        _serverSide.logHSResp(hsResp);

        shareKey = _serverSide.makeShareKey(hsReq->dh_pubkey);
        std::string respData = _serverSide.encryptHSResp(hsResp, hsReq->tmp_sym_sec);
        delete hsReq;
        delete hsResp;
        return respData;
    }

private:
    handshake_helper _serverSide;
    std::mutex _lockHandshake;
};

void GateUser::onRequest(const lymsg_header* reqHeader, const std::string& reqData) {
    GateServer* gateServer = dynamic_cast<GateServer*>(_baseServer);
    if (reqHeader->type == LYMSG_TYPE_HANDSHAKE) {
        std::string shareKey;
        std::string respData = gateServer->onHandshake(reqData, shareKey);
        if (respData.empty()) { // handshake fail
            finHandshake(false);
            LOG_ERR_MSG("handshake fail, reply empty string");
            tcp_message* ack = NetUser::createResponse(reqHeader, std::string());
            this->post(ack);
        } else {
            finHandshake(true, shareKey);
            LOG_MSG(LogLevel::Debug, "handshake success, response size=%d", respData.size());
            tcp_message* resp = NetUser::createResponse(reqHeader, respData);
            this->post(resp);
        }
    } else {
        std::string plainReqData;
        if (reqHeader->param & LYMSG_PARAM_ENC) { // encrypt msg
            if (isHandshake()) {
                LOG_MSG(LogLevel::TraceMore, "decrypt request, size=%d", reqData.size());
                plainReqData = decrypt(reqData);
            } else {
                LOG_ERR_MSG("reject request before handshake, size=%d", reqData.size());
                return;
            }
        } else { // plain msg
            LOG_MSG(LogLevel::TraceMore, "plain request, size=%d", reqData.size());
            plainReqData = reqData;
        }
        NetUser::onRequest(reqHeader, plainReqData);
    }
}

tcp_message* GateUser::createResponse(const lymsg_header* reqHeader, const std::string& respData) {
    std::string packData;
    if (reqHeader->param & LYMSG_PARAM_ENC) {
        if (isHandshake()) {
            LOG_MSG(LogLevel::TraceMore, "encrypt response, size=%d", respData.size());
            packData = encrypt(respData);
        } else {
            LOG_ERR_MSG("unexpected response before handshake, size=%d", respData.size());
            packData = "";
        }
    } else {
        LOG_MSG(LogLevel::TraceMore, "plain response, size=%d", respData.size());
        packData = respData;
    }
    return NetUser::createResponse(reqHeader, packData);
}


}
