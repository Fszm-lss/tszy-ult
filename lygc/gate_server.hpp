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
    void setHandshakeSuccess(const std::string& shareKey, openssl_utils::eEncryptType sessionType) {
        if (shareKey.length() >= 32) {
            _secret  = shareKey.substr(0, 16);
            _ivec    = shareKey.substr(16, 16);
            _enctype = sessionType;
            _handshakeFin.store(true);
        } else {
            _handshakeFin.store(false);
            LOG_ERR_MSG("setHandshakeSuccess error, shareKey invalid, len=%d", shareKey.length());
        }
    }

    void setHandshakeFail() {
        _handshakeFin.store(false);
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

    bool isHandshakeDone() {
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

    void setSessionSecType(openssl_utils::eEncryptType t) { _session_symsec_type = t; }
    openssl_utils::eEncryptType getSessionSecType() const { return _session_symsec_type; }

    std::string onHandshake(const std::string& reqData, std::string& shareKey) {
        std::lock_guard<std::mutex> lock(_lockHandshake);
        HandshakeReq* hsReq = _serverSide.decryptHSReq(reqData);
        if (!hsReq) {
            return std::string();
        }
        _serverSide.logHSReq(hsReq);

        HandshakeResp* hsResp = _serverSide.creatHSResp(hsReq, _session_symsec_type);
        if (!hsResp) {
            delete hsReq;
            return std::string();
        }
        _serverSide.logHSResp(hsResp);

        shareKey = _serverSide.makeShareKey(hsReq->getPeerPubkey());
        std::string respData = _serverSide.encryptHSResp(hsResp, hsReq->temp_symsec, (openssl_utils::eEncryptType)hsReq->temp_symsec_type);
        delete hsReq;
        delete hsResp;
        return respData;
    }

private:
    handshake_helper _serverSide;
    std::mutex _lockHandshake;
    openssl_utils::eEncryptType _session_symsec_type{openssl_utils::eEncryptType::eET_AES_CFB};
};

void GateUser::onRequest(const lymsg_header* reqHeader, const std::string& reqData) {
    GateServer* gateServer = dynamic_cast<GateServer*>(_baseServer);
    if (reqHeader->type == LYMSG_TYPE_HANDSHAKE) {
        std::string shareKey;
        std::string respData = gateServer->onHandshake(reqData, shareKey);
        if (respData.empty()) { // handshake fail
            setHandshakeFail();
            LOG_ERR_MSG("handshake fail, reply empty string");
            this->post(std::unique_ptr<tcp_message>(NetUser::createResponse(reqHeader, std::string())));
        } else {
            setHandshakeSuccess(shareKey, gateServer->getSessionSecType());
            LOG_MSG(LogLevel::Debug, "handshake success, response size=%d", respData.size());
            this->post(std::unique_ptr<tcp_message>(NetUser::createResponse(reqHeader, respData)));
        }
    } else {
        std::string plainReqData;
        if (reqHeader->type & LYMSG_TYPE_ENC) { // encrypt msg
            if (isHandshakeDone()) {
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
    if (reqHeader->type & LYMSG_TYPE_ENC) {
        if (isHandshakeDone()) {
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
