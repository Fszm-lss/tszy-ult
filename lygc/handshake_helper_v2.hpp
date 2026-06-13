#pragma once

#include "fszm/openssl_utils_v2.hpp"
#include "fszm/random_utils.hpp"
#include "fszm/serialize_helper.hpp"
#include "zbf/mem_alloc.hpp"

namespace lygc {

using fszm::openssl_utils;
using fszm::random_utils;
using fszm::serialize_helper;
using zbf::LogLevel;
using zbf::object_tracker;

enum KeyExchangeType : uint8_t {
    KX_DH   = 0,
    KX_ECDH = 1,
};

struct DHExchangeReq {
    std::string p;
    std::string g;
    std::string pubkey;

    void serialize(serialize_helper& helper) const {
        helper.serialize_str(p);
        helper.serialize_str(g);
        helper.serialize_str(pubkey);
    }

    static DHExchangeReq deserialize(serialize_helper& helper) {
        DHExchangeReq req;
        req.p      = helper.deserialize_str();
        req.g      = helper.deserialize_str();
        req.pubkey = helper.deserialize_str();
        return req;
    }
};

struct DHExchangeResp {
    std::string pubkey;

    void serialize(serialize_helper& helper) const {
        helper.serialize_str(pubkey);
    }

    static DHExchangeResp deserialize(serialize_helper& helper) {
        DHExchangeResp resp;
        resp.pubkey = helper.deserialize_str();
        return resp;
    }
};

struct ECDHExchangeReq {
    uint32_t    curve_nid;
    std::string pubkey;

    void serialize(serialize_helper& helper) const {
        helper.serialize_int32(curve_nid);
        helper.serialize_str(pubkey);
    }

    static ECDHExchangeReq deserialize(serialize_helper& helper) {
        ECDHExchangeReq req;
        req.curve_nid = helper.deserialize_int32();
        req.pubkey    = helper.deserialize_str();
        return req;
    }
};

struct ECDHExchangeResp {
    std::string pubkey;

    void serialize(serialize_helper& helper) const {
        helper.serialize_str(pubkey);
    }

    static ECDHExchangeResp deserialize(serialize_helper& helper) {
        ECDHExchangeResp resp;
        resp.pubkey = helper.deserialize_str();
        return resp;
    }
};

// client to server
struct HandshakeReq : object_tracker<HandshakeReq> {
    KeyExchangeType  kx_type;
    DHExchangeReq    dh;
    ECDHExchangeReq  ecdh;
    // AES128 [key 16bytes] [ivec 16bytes]
    std::string temp_symsec;
    uint8_t     temp_symsec_type;

    std::string getPeerPubkey() const {
        if (kx_type == KX_ECDH) return ecdh.pubkey;
        return dh.pubkey;
    }

    std::string serialize() const {
        serialize_helper helper;
        helper.serialize_uint8(kx_type);
        if (kx_type == KX_ECDH) {
            ecdh.serialize(helper);
        } else {
            dh.serialize(helper);
        }
        helper.serialize_str(temp_symsec);
        helper.serialize_uint8(temp_symsec_type);
        return helper.data();
    }

    static HandshakeReq* deserialize(const std::string& data) {
        serialize_helper helper(data);
        HandshakeReq* req = new HandshakeReq;
        req->kx_type = (KeyExchangeType)helper.deserialize_uint8();
        if (req->kx_type == KX_ECDH) {
            req->ecdh = ECDHExchangeReq::deserialize(helper);
        } else {
            req->dh = DHExchangeReq::deserialize(helper);
        }
        req->temp_symsec = helper.deserialize_str();
        req->temp_symsec_type = helper.deserialize_uint8();
        return req;
    }
};

// server to client
struct HandshakeResp : object_tracker<HandshakeResp> {
    KeyExchangeType   kx_type;
    DHExchangeResp    dh;
    ECDHExchangeResp  ecdh;
    uint8_t           session_symsec_type;

    std::string getPeerPubkey() const {
        if (kx_type == KX_ECDH) return ecdh.pubkey;
        return dh.pubkey;
    }

    std::string serialize() const {
        serialize_helper helper;
        helper.serialize_uint8(kx_type);
        if (kx_type == KX_ECDH) {
            ecdh.serialize(helper);
        } else {
            dh.serialize(helper);
        }
        helper.serialize_uint8(session_symsec_type);
        return helper.data();
    }

    static HandshakeResp* deserialize(const std::string& data) {
        serialize_helper helper(data);
        HandshakeResp* resp = new HandshakeResp;
        resp->kx_type = (KeyExchangeType)helper.deserialize_uint8();
        if (resp->kx_type == KX_ECDH) {
            resp->ecdh = ECDHExchangeResp::deserialize(helper);
        } else {
            resp->dh = DHExchangeResp::deserialize(helper);
        }
        resp->session_symsec_type = helper.deserialize_uint8();
        return resp;
    }
};

class handshake_helper {
public:
    handshake_helper() : _dh(nullptr), _ecKey(nullptr), _rsaKey(nullptr) {
    }

    ~handshake_helper() {
        if (_dh) {
            openssl_utils::dh_destroy(&_dh);
        }
        if (_ecKey) {
            openssl_utils::ecc_destroy(&_ecKey);
        }
        if (_rsaKey) {
            openssl_utils::free_rsa_key(&_rsaKey);
        }
    }

    bool loadRSAKey(const std::string& rsaKeyPath, bool pub) {
        _rsaKey = openssl_utils::load_rsa_key(rsaKeyPath.c_str(), pub);
        if (!_rsaKey) {
            LOG_ERR_MSG("load_rsa_key fail, path=%s", rsaKeyPath.c_str());
            return false;
        }
        return true;
    }

    // client - step1
    // prime_len: DH bits (512~4096) or ECDH curve_nid (e.g. NID_X9_62_prime256v1 = 415)
    HandshakeReq* creatHSReq(KeyExchangeType kx_type = KX_ECDH, int prime_len = NID_X9_62_prime256v1, openssl_utils::eEncryptType tempType = openssl_utils::eEncryptType::eET_AES_CBC) {
        if (tempType < openssl_utils::eEncryptType::eET_AES_ECB || tempType > openssl_utils::eEncryptType::eET_AES_CTR) {
            LOG_ERR_MSG("tempType invalid, only AES supported: %d", tempType);
            return nullptr;
        }

        _kx_type = kx_type;
        HandshakeReq* req = new HandshakeReq;
        req->kx_type = _kx_type;

        if (_kx_type == KX_ECDH) {
            if (openssl_utils::ecc_create(prime_len, &_ecKey) != 1) {
                LOG_ERR_MSG("ecc_create fail, curve_nid=%d", prime_len);
                delete req;
                return nullptr;
            }
            req->ecdh.curve_nid = prime_len;
            req->ecdh.pubkey    = openssl_utils::ecc_export_pubkey(_ecKey);
            _ec_curve_nid       = prime_len;
        } else {
            if (prime_len < 512 || prime_len > 4096) {
                LOG_ERR_MSG("prime_len(512~4096) invalid: %d", prime_len);
                delete req;
                return nullptr;
            }
            _dh = openssl_utils::dh_generate(prime_len);
            if (!_dh) {
                LOG_ERR_MSG("dh_generate fail, prime_len=%d", prime_len);
                delete req;
                return nullptr;
            }
            req->dh.p      = openssl_utils::dh_export(_dh, "p");
            req->dh.g      = openssl_utils::dh_export(_dh, "g");
            req->dh.pubkey = openssl_utils::dh_export(_dh, "pub");
        }

        req->temp_symsec = random_utils::randomString(fszm::NumbersAndLetters, 32);
        req->temp_symsec_type = tempType;
        return req;
    }

    // client - step2
    std::string encryptHSReq(HandshakeReq* req) {
        std::string result;
        std::string data = req->serialize();
        LOG_MSG(LogLevel::Trace, "HandshakeReq serialize size=%d", data.size());
        size_t len = rsaEncrypt(data, true, result);
        if (len > 0) {
            LOG_MSG(LogLevel::Trace, "rsa encrypt success, src size=%d, dst size=%d", data.size(), len);
        } else {
            LOG_ERR_MSG("rsa encrypt fail, src size=%d", data.size());
        }
        return result;
    }

    // server - step1
    HandshakeReq* decryptHSReq(const std::string& data) {
        HandshakeReq* req = nullptr;
        std::string result;
        size_t len = rsaEncrypt(data, false, result);
        if (len > 0) {
            LOG_MSG(LogLevel::Trace, "rsa decrypt success, src size=%d, dst size=%d", data.size(), len);
            req = HandshakeReq::deserialize(result);
        } else {
            LOG_ERR_MSG("rsa decrypt fail, src size=%d", data.size());
        }
        return req;
    }

    // server - step2
    HandshakeResp* creatHSResp(const HandshakeReq* req, openssl_utils::eEncryptType sessionType = openssl_utils::eEncryptType::eET_AES_CFB) {
        if (sessionType < openssl_utils::eEncryptType::eET_AES_ECB || sessionType > openssl_utils::eEncryptType::eET_AES_CTR) {
            LOG_ERR_MSG("sessionType invalid, only AES supported: %d", sessionType);
            return nullptr;
        }

        HandshakeResp* resp = new HandshakeResp;
        resp->kx_type = req->kx_type;
        _kx_type = req->kx_type;  // derived from client's handshake request

        if (req->kx_type == KX_ECDH) {
            if (openssl_utils::ecc_create(req->ecdh.curve_nid, &_ecKey) != 1) {
                LOG_ERR_MSG("ecc_create fail, curve_nid=%d", req->ecdh.curve_nid);
                delete resp;
                return nullptr;
            }
            resp->ecdh.pubkey = openssl_utils::ecc_export_pubkey(_ecKey);
            _ec_curve_nid = req->ecdh.curve_nid;
        } else {
            _dh = openssl_utils::dh_generate(req->dh.p, req->dh.g);
            if (!_dh) {
                LOG_ERR_MSG("dh_generate fail");
                delete resp;
                return nullptr;
            }
            resp->dh.pubkey = openssl_utils::dh_export(_dh, "pub");
        }

        resp->session_symsec_type = sessionType;
        return resp;
    }

    // server - step3
    std::string encryptHSResp(const HandshakeResp* resp, const std::string& temp_symsec, openssl_utils::eEncryptType encType) {
        std::string in_str = resp->serialize();
        LOG_MSG(LogLevel::Trace, "HandshakeResp serialize size=%d", in_str.size());

        std::string in_key  = temp_symsec.substr(0, 16);
        std::string in_ivec = temp_symsec.substr(16);
        std::string result;
        int rc = openssl_utils::symmetric_encrypt(in_str, in_key, in_ivec, encType, true, result);
        if (rc) {
            LOG_ERR_MSG("symmetric encrypt fail, rc=%d", rc);
        }
        return result;
    }

    // client - step3
    HandshakeResp* decryptHSResp(const std::string& data, const std::string& temp_symsec, openssl_utils::eEncryptType encType) {
        std::string in_str  = data;
        std::string in_key  = temp_symsec.substr(0, 16);
        std::string in_ivec = temp_symsec.substr(16);
        HandshakeResp* resp = nullptr;
        std::string result;
        int rc = openssl_utils::symmetric_encrypt(in_str, in_key, in_ivec, encType, false, result);
        if (rc) {
            LOG_ERR_MSG("symmetric decrypt fail, rc=%d", rc);
        } else {
            resp = HandshakeResp::deserialize(result);
        }
        return resp;
    }

    // both - step4
    std::string makeShareKey(const std::string& peer_pubkey) {
        std::string strShareKey;
        EVP_PKEY* key = (_kx_type == KX_ECDH) ? _ecKey : _dh;
        int keyLen = EVP_PKEY_size(key);
        if (keyLen > 0) {
            unsigned char* shrKey = (unsigned char*) OPENSSL_malloc(keyLen);
            int len = 0;
            if (_kx_type == KX_ECDH) {
                len = openssl_utils::ecc_compute_sharekey(_ecKey, _ec_curve_nid, peer_pubkey, shrKey, keyLen);
            } else {
                len = openssl_utils::dh_compute_sharekey(_dh, peer_pubkey, shrKey, keyLen);
            }
            if (len > 0) {
                strShareKey = std::string((const char*)shrKey, len);
            } else {
                LOG_ERR_MSG("compute sharekey fail, kx_type=%d, len=%d", _kx_type, len);
            }
            OPENSSL_free(shrKey);
        } else {
            LOG_ERR_MSG("EVP_PKEY_size fail, keyLen=%d", keyLen);
        }
        log(strShareKey, "Share Key");
        return strShareKey;
    }

public:
    std::string hex(const std::string& data) {
        std::string str = openssl_utils::hex2string((unsigned char*)data.data(), data.size());
        return str;
    }

    void log(const std::string& str, const char* tag) {
        LOG_MSG(LogLevel::Trace, "%s=%s", tag, openssl_utils::base64_encode(str).c_str());
        // LOG_MSG(LogLevel::Trace, "%s=%s", tag, hex(str).c_str());
    }

    void logHSReq(const HandshakeReq* req) {
        if (req->kx_type == KX_ECDH) {
            LOG_MSG(LogLevel::Trace, "HSReq, kx_type=ECDH, curve_nid=%d", req->ecdh.curve_nid);
            log(req->ecdh.pubkey, "HSReq, PUB");
        } else {
            LOG_MSG(LogLevel::Trace, "HSReq, kx_type=DH");
            log(req->dh.p,        "HSReq, (P)");
            log(req->dh.g,        "HSReq, (G)");
            log(req->dh.pubkey,   "HSReq, PUB");
        }
        // log(req->temp_symsec, "HSReq, TMP");
    }

    void logHSResp(const HandshakeResp* resp) {
        log(resp->getPeerPubkey(), "HSResp, PUB");
    }

    size_t rsaEncrypt(const std::string& data, bool enc, std::string& result) {
        int rsa_len = EVP_PKEY_size(_rsaKey);
        int block_in = enc ? (rsa_len - 11) : rsa_len;
        openssl_utils::eEncryptType encType = enc ?
            openssl_utils::eEncryptType::eET_RSA_PUBLIC_ENC : openssl_utils::eEncryptType::eET_RSA_PRIVATE_DEC;
        
        result.clear();
        int times = (data.size() + block_in - 1) / block_in;
        for (int i = 0; i < times; ++i) {
            unsigned char* dst = nullptr;
            unsigned int dstlen = 0;
            std::string src = data.substr(i*block_in, (i==times-1) ? (data.size()-i*block_in) : block_in);
            int rc = openssl_utils::rsa_encrypt((unsigned char*)src.data(), src.size(), _rsaKey, encType, &dst, &dstlen);
            if (rc > 0) {
                result += std::string((const char*)dst, rc);
                free(dst);
            } else {
                if (dst) free(dst);
                result.clear();
                break;
            }
        }
        return result.size();
    }

private:
    EVP_PKEY* _dh;
    EVP_PKEY* _ecKey;
    EVP_PKEY* _rsaKey;
    // client set it by creatHSReq(), server derive it from creatHSResp()
    KeyExchangeType _kx_type{KX_ECDH};
    // ECDH only, client set it by creatHSReq(), server derive it from creatHSResp()
    int _ec_curve_nid{0};
};

}
