
#include "zbf/log_utils.hpp"
#include "lygc/handshake_helper_v2.hpp"

using namespace zbf;
using namespace lygc;

int main(int argc, char** argv)
{
    int prime_len = 512;
    if (argc >= 2 && atoi(argv[1])) {
        prime_len = atoi(argv[1]);
    }

    zbf::log_utils::log_level = LogLevel::TraceMore;
    LOG_MSG(LogLevel::Info, "main start, prime_len=%d", prime_len);

    handshake_helper client;
    handshake_helper server;
    client.loadRSAKey("key/rsa_public_key.pem", true);
    server.loadRSAKey("key/rsa_private_key.pem", false);
    
    // 1
    HandshakeReq* cltHSReq = client.creatHSReq(prime_len);
    if (!cltHSReq) return 1;
    client.logHSReq(cltHSReq);
    std::string binHSReq = client.encryptHSReq(cltHSReq);
    LOG_MSG(LogLevel::Info, "binHSReq size=%d", binHSReq.size());
    
    // 2
    HandshakeReq* svrHSReq = server.decryptHSReq(binHSReq);
    server.logHSReq(svrHSReq);
    HandshakeResp* svrHSResp = server.creatHSResp(svrHSReq);
    server.logHSResp(svrHSResp);
    std::string binHSResp = server.encryptHSResp(svrHSResp, svrHSReq->tmp_sym_sec);
    LOG_MSG(LogLevel::Info, "binHSResp size=%d", binHSResp.size());    

    // 3
    HandshakeResp* cltHSResp = client.decryptHSResp(binHSResp, cltHSReq->tmp_sym_sec);
    client.logHSResp(cltHSResp);

    std::string svrShareKey = server.makeShareKey(svrHSReq->dh_pubkey);
    std::string cltShareKey = client.makeShareKey(cltHSResp->dh_pubkey);

    if (svrShareKey.compare(cltShareKey) == 0) {
        LOG_MSG(LogLevel::Info, "ShareKey equal, size=%d", svrShareKey.size());
    } else {
        LOG_MSG(LogLevel::Info, "ShareKey not equal");
    }

    delete cltHSReq;
    delete cltHSResp;
    delete svrHSReq;
    delete svrHSResp;
    LOG_MSG(LogLevel::Fatal, "main exit");
    return 0;
}
