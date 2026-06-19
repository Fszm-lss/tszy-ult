
#include "fszm/random_utils.hpp"
#include "lygc/lymsg_protocol.hpp"
#include "lygc/handshake_helper_v2.hpp"
#include "lydefine.h"
#include "nlohmann/json.hpp"
#include <openssl/obj_mac.h>

using json = nlohmann::json;
using namespace zbf;
using namespace lygc;
using namespace fszm;

async_client_manager* g_asyncMgr = nullptr;

class common_client : public tcpsock_ha_asynclt {
public:
    common_client() : tcpsock_ha_asynclt() {
        _hsSuccess = false;
        _hsFail = false;
        _enctype = openssl_utils::eEncryptType::eET_AES_CFB;
    }

	void onResponse(const tcp_message* msg) override {
        lymsg_header respHeader;
        std::string respData;
        lymsg_helper::unpackMsg(msg, respHeader, respData);
        if (respHeader.type == (LYMSG_TYPE_HANDSHAKE|LYMSG_TYPE_RESP)) {
            onHandshake(respData);
        } else {
            std::string result;
            bool success = decrypt(respData, result);
            if (!success) {
                result = respData;
            }
            LOG_MSG(LogLevel::Info, "onResponse: msg=%s, data=%s", LYMSG_DESC(&respHeader).c_str(), lygc::strBrief(result).c_str());
        }
    }

    bool handshake(const std::string& keyPath) {
        if (!_clientSide.loadRSAKey(keyPath, true)) return false;

        HandshakeReq* hsReq = _clientSide.creatHSReq(KX_ECDH, NID_X9_62_prime256v1);
        if (!hsReq) return false;

        _clientSide.logHSReq(hsReq);
        std::string binHSReq = _clientSide.encryptHSReq(hsReq);
        if (binHSReq.empty()) {
            delete hsReq;
            return false;
        }

        lymsg_header header;
        header.origin = LYGATE_CLIENT_ORGIN;
        header.type = LYMSG_TYPE_HANDSHAKE;
        header.serial = 0;

        auto msg = std::unique_ptr<tcp_message>(lymsg_helper::packMsg(&header, binHSReq));
        int rc = post(std::move(msg));
        if (rc) {
            LOG_ERR_MSG("post handshake request fail, rc=%d", rc);
            delete hsReq;
            return false;
        }
        LOG_MSG(LogLevel::Info, "post handshake request success");
        _temp_symsec = hsReq->temp_symsec;
        _temp_symsec_type = (openssl_utils::eEncryptType)hsReq->temp_symsec_type;
        delete hsReq;
        return true;
    }

    void onHandshake(const std::string& binHSResp) {
        if (binHSResp.empty()) {
            LOG_ERR_MSG("handshake fail, response empty");
            _hsFail = true;
            return;
        }

        HandshakeResp* hsResp = _clientSide.decryptHSResp(binHSResp, _temp_symsec, _temp_symsec_type);
        if (!hsResp) {
            LOG_ERR_MSG("handshake fail, decrypt response error");
            _hsFail = true;
            return;
        }

        _clientSide.logHSResp(hsResp);
        std::string shareKey = _clientSide.makeShareKey(hsResp->getPeerPubkey());
        _secret  = shareKey.substr(0, 16);
        _ivec    = shareKey.substr(16, 16);
        _enctype = (openssl_utils::eEncryptType)hsResp->session_symsec_type;
        _hsSuccess = true;
        delete hsResp;
        LOG_MSG(LogLevel::Info, "handshake success");
    }

    bool handshakeDone() {
        return _hsSuccess || _hsFail;
    }

    int encrypt(const std::string& data, std::string& result) {
        if (!_hsSuccess) return false;
        int rc = openssl_utils::symmetric_encrypt(data, _secret, _ivec, _enctype, true, result);
        if (rc) {
            LOG_ERR_MSG("symmetric encrypt fail, rc=%d, size=%d", rc, data.size());
            return false;
        }
        return true;
    }

    int decrypt(const std::string& data, std::string& result) {
        if (!_hsSuccess) return false;
        int rc = openssl_utils::symmetric_encrypt(data, _secret, _ivec, _enctype, false, result);
        if (rc) {
            LOG_ERR_MSG("symmetric decrypt fail, rc=%d, size=%d", rc, data.size());
            return false;
        }
        return true;
    }

private:
    handshake_helper _clientSide;
    bool _hsSuccess;
    bool _hsFail;
    std::string _temp_symsec;
    openssl_utils::eEncryptType _temp_symsec_type;
    std::string _secret;
    std::string _ivec;
    openssl_utils::eEncryptType _enctype;
};

void worker(void* param) {
    common_client* client = (common_client*) param;
    lymsg_header header = {0};
    int length = fszm::random_utils::randomNumber(16, 32);
    std::string randstr = fszm::random_utils::randomString(fszm::NumbersAndLetters, length);
    for (int i = 1; i <= 10; ++i) {
        header.origin = LYGATE_CLIENT_ORGIN;
        header.type = 0x20;        
        header.type |= LYMSG_TYPE_ENC;
        header.serial = i;

        json jobj;
        jobj["msg"] = randstr;
        std::string jsonstr = jobj.dump();
        std::string result;
        bool success = client->encrypt(jsonstr, result);
        if (!success) {
            header.type &= ~LYMSG_TYPE_ENC;
            result = jsonstr;
        }

        header.size = result.size();
        auto msg = std::unique_ptr<tcp_message>(lymsg_helper::packMsg(&header, result));
        int rc = client->post(std::move(msg));
        if (rc) {
            LOG_ERR_MSG("post fail: msg=%s, rc=%d", LYMSG_DESC(&header).c_str(), rc);
        } else {
            LOG_MSG(LogLevel::Info, "post success: msg=%s, rc=%d", LYMSG_DESC(&header).c_str(), rc);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    g_asyncMgr->stop();
}

void onExit() {
    zbf::logMemTrackStat();
}

int main(int argc, char** argv)
{
    std::atexit(onExit);
    std::string log_path = log_utils::createLogPath();
    log_utils::open(log_path.c_str(), LogLevel::Trace);

    lymsg_protocol* proto = new lymsg_protocol;
    async_client_manager cltMgr(proto);
    g_asyncMgr = &cltMgr;
    common_client* client = new common_client();
    client->addConnect("127.0.0.1", 35105, proto);
    cltMgr.manageClient(client);
    cltMgr.start(1);

    std::thread* thd = new std::thread([&](){
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        client->handshake("key/rsa_public_key.pem");

        while (!client->handshakeDone()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        worker(client);
    });

    cltMgr.serveUtilStop();
    if (thd->joinable()) {
        thd->join();
    }
    delete thd;
    
    log_utils::close();
    return 0;
}
