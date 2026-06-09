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


// client to server
struct HandshakeReq : object_tracker<HandshakeReq> {
    std::string dh_p;
    std::string dh_g;
    std::string dh_pubkey;
    // AES128 [key 16bytes] [ivec 16bytes]
    std::string tmp_sym_sec;

    std::string serialize() const {
        serialize_helper helper;
        helper.serialize_str(dh_p);
        helper.serialize_str(dh_g);
        helper.serialize_str(dh_pubkey);
        helper.serialize_str(tmp_sym_sec);
        return helper.data();
    }

    static HandshakeReq* deserialize(const std::string& data) {
        serialize_helper helper(data);
        HandshakeReq* req = new HandshakeReq;
        req->dh_p        = helper.deserialize_str();
        req->dh_g        = helper.deserialize_str();
        req->dh_pubkey   = helper.deserialize_str();
        req->tmp_sym_sec = helper.deserialize_str();
        return req;
    }
};

// server to client
struct HandshakeResp : object_tracker<HandshakeResp> {
    std::string dh_pubkey;

    std::string serialize() const {
        serialize_helper helper;
        helper.serialize_str(dh_pubkey);
        return helper.data();
    }

    static HandshakeResp* deserialize(const std::string& data) {
        serialize_helper helper(data);
        HandshakeResp* resp = new HandshakeResp;
        resp->dh_pubkey = helper.deserialize_str();
        return resp;
    }
};

class handshake_helper {
public:
    handshake_helper() : _dh(nullptr), _rsaKey(nullptr) {
        tmp_sym_sec_type = openssl_utils::eEncryptType::eET_AES_CBC;
        // tmp_sym_sec_type = openssl_utils::eEncryptType::eET_AES_CFB;
        // tmp_sym_sec_type = openssl_utils::eEncryptType::eET_AES_OFB;
        // tmp_sym_sec_type = openssl_utils::eEncryptType::eET_AES_CTR;
    }

    ~handshake_helper() {
        if (_dh) {
            openssl_utils::dh_destroy(&_dh);
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
    HandshakeReq* creatHSReq(int prime_len = 1024) {
        // at least 512
        if (prime_len < 512 || prime_len > 4096) {
            LOG_ERR_MSG("prime_len(512~4096) invalid: %d", prime_len);
            return nullptr;
        }

        _dh = openssl_utils::dh_generate(prime_len);
        if (!_dh) {
            LOG_ERR_MSG("dh_generate fail");
            return nullptr;
        }

        HandshakeReq* req = new HandshakeReq;
        req->dh_p      = openssl_utils::dh_export(_dh, "p");
        req->dh_g      = openssl_utils::dh_export(_dh, "g");
        req->dh_pubkey = openssl_utils::dh_export(_dh, "pub");
        req->tmp_sym_sec = random_utils::randomString(fszm::NumbersAndLetters, 32);
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
    HandshakeResp* creatHSResp(const HandshakeReq* req) {
        _dh = openssl_utils::dh_generate(req->dh_p, req->dh_g);
        if (!_dh) {
            LOG_ERR_MSG("dh_generate fail");
            return nullptr;
        }

        HandshakeResp* resp = new HandshakeResp;
        resp->dh_pubkey = openssl_utils::dh_export(_dh, "pub");
        return resp;
    }

    // server - step3
    std::string encryptHSResp(const HandshakeResp* resp, const std::string& tmp_sym_sec) {
        std::string in_str = resp->serialize();
        LOG_MSG(LogLevel::Trace, "HandshakeResp serialize size=%d", in_str.size());

        std::string in_key = tmp_sym_sec.substr(0, 16);
        std::string in_ivec = tmp_sym_sec.substr(16);
        std::string result;
        int rc = openssl_utils::symmetric_encrypt(in_str, in_key, in_ivec, tmp_sym_sec_type, true, result);
        if (rc) {
            LOG_ERR_MSG("symmetric encrypt fail, rc=%d", rc);
        }
        return result;
    }

    // client - step3
    HandshakeResp* decryptHSResp(const std::string& data, const std::string& tmp_sym_sec) {
        std::string in_str = data;
        std::string in_key = tmp_sym_sec.substr(0, 16);
        std::string in_ivec = tmp_sym_sec.substr(16);
        HandshakeResp* resp = nullptr;
        std::string result;
        int rc = openssl_utils::symmetric_encrypt(in_str, in_key, in_ivec, tmp_sym_sec_type, false, result);
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
        int keyLen = EVP_PKEY_size(_dh);
        if (keyLen > 0) {
            unsigned char* shrKey = (unsigned char*) OPENSSL_malloc(keyLen);
            int len = openssl_utils::dh_compute_sharekey(_dh, peer_pubkey, shrKey, keyLen);
            if (len > 0) {
                strShareKey = std::string((const char*)shrKey, len);
            } else {
                LOG_ERR_MSG("dh_compute_sharekey fail, len=%d", len);
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
        log(req->dh_p,        "HSReq, (P)");
        log(req->dh_g,        "HSReq, (G)");
        log(req->dh_pubkey,   "HSReq, PUB");
        // log(req->tmp_sym_sec, "HSReq, TMP");
    }

    void logHSResp(const HandshakeResp* resp) {
        log(resp->dh_pubkey,  "HSResp, PUB");
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
    EVP_PKEY* _rsaKey;
    openssl_utils::eEncryptType tmp_sym_sec_type;
};

}
