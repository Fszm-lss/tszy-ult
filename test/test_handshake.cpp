
#include "zbf/log_utils.hpp"
#include "lygc/handshake_helper_v2.hpp"
#include <openssl/obj_mac.h>

using namespace zbf;
using namespace lygc;

void testDH(int prime_len) {
    LOG_MSG(LogLevel::Info, "=== DH test, prime_len=%d ===", prime_len);
    handshake_helper client;
    handshake_helper server;
    client.loadRSAKey("key/rsa_public_key.pem", true);
    server.loadRSAKey("key/rsa_private_key.pem", false);

    // 1
    HandshakeReq* cltHSReq = client.creatHSReq(KX_DH, prime_len, openssl_utils::eEncryptType::eET_AES_OFB);
    if (!cltHSReq) return;
    client.logHSReq(cltHSReq);
    std::string binHSReq = client.encryptHSReq(cltHSReq);
    LOG_MSG(LogLevel::Info, "binHSReq size=%d", binHSReq.size());

    // 2
    HandshakeReq* svrHSReq = server.decryptHSReq(binHSReq);
    server.logHSReq(svrHSReq);
    HandshakeResp* svrHSResp = server.creatHSResp(svrHSReq, openssl_utils::eEncryptType::eET_AES_CTR);
    server.logHSResp(svrHSResp);
    std::string binHSResp = server.encryptHSResp(svrHSResp, svrHSReq->temp_symsec, (openssl_utils::eEncryptType)svrHSReq->temp_symsec_type);
    LOG_MSG(LogLevel::Info, "binHSResp size=%d", binHSResp.size());

    // 3
    HandshakeResp* cltHSResp = client.decryptHSResp(binHSResp, cltHSReq->temp_symsec, (openssl_utils::eEncryptType)cltHSReq->temp_symsec_type);
    client.logHSResp(cltHSResp);

    std::string svrShareKey = server.makeShareKey(svrHSReq->getPeerPubkey());
    std::string cltShareKey = client.makeShareKey(cltHSResp->getPeerPubkey());

    if (svrShareKey.compare(cltShareKey) == 0) {
        LOG_MSG(LogLevel::Info, "ShareKey equal, size=%d", svrShareKey.size());
    } else {
        LOG_MSG(LogLevel::Info, "ShareKey not equal");
    }

    LOG_MSG(LogLevel::Info, "svrHSResp.session_symsec_type=%d", svrHSResp->session_symsec_type);

    delete cltHSReq;
    delete cltHSResp;
    delete svrHSReq;
    delete svrHSResp;
}

void testECDH(int curve_nid) {
    LOG_MSG(LogLevel::Info, "=== ECDH test, curve_nid=%d ===", curve_nid);
    handshake_helper client;
    handshake_helper server;
    client.loadRSAKey("key/rsa_public_key.pem", true);
    server.loadRSAKey("key/rsa_private_key.pem", false);

    // 1
    HandshakeReq* cltHSReq = client.creatHSReq(KX_ECDH, curve_nid, openssl_utils::eEncryptType::eET_AES_CBC);
    if (!cltHSReq) return;
    client.logHSReq(cltHSReq);
    std::string binHSReq = client.encryptHSReq(cltHSReq);
    LOG_MSG(LogLevel::Info, "binHSReq size=%d", binHSReq.size());

    // 2
    HandshakeReq* svrHSReq = server.decryptHSReq(binHSReq);
    server.logHSReq(svrHSReq);
    HandshakeResp* svrHSResp = server.creatHSResp(svrHSReq, openssl_utils::eEncryptType::eET_AES_CFB);
    server.logHSResp(svrHSResp);
    std::string binHSResp = server.encryptHSResp(svrHSResp, svrHSReq->temp_symsec, (openssl_utils::eEncryptType)svrHSReq->temp_symsec_type);
    LOG_MSG(LogLevel::Info, "binHSResp size=%d", binHSResp.size());

    // 3
    HandshakeResp* cltHSResp = client.decryptHSResp(binHSResp, cltHSReq->temp_symsec, (openssl_utils::eEncryptType)cltHSReq->temp_symsec_type);
    client.logHSResp(cltHSResp);

    std::string svrShareKey = server.makeShareKey(svrHSReq->getPeerPubkey());
    std::string cltShareKey = client.makeShareKey(cltHSResp->getPeerPubkey());

    if (svrShareKey.compare(cltShareKey) == 0) {
        LOG_MSG(LogLevel::Info, "ShareKey equal, size=%d", svrShareKey.size());
    } else {
        LOG_MSG(LogLevel::Info, "ShareKey not equal");
    }

    LOG_MSG(LogLevel::Info, "svrHSResp.session_symsec_type=%d", svrHSResp->session_symsec_type);

    delete cltHSReq;
    delete cltHSResp;
    delete svrHSReq;
    delete svrHSResp;
}

int main(int argc, char** argv)
{
    zbf::log_utils::log_level = LogLevel::TraceMore;
    LOG_MSG(LogLevel::Info, "main start");

    testDH(1024);
    testECDH(NID_X9_62_prime256v1);
    testECDH(NID_secp384r1);

    LOG_MSG(LogLevel::Fatal, "main exit");
    return 0;
}
