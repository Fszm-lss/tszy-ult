#ifndef OPENSSL_UTILS_V2_HPP
#define OPENSSL_UTILS_V2_HPP
// use -lssl -lcrypto while link

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/dh.h>

#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/core_names.h>
#include <openssl/opensslv.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
	#include <openssl/provider.h>
#endif

namespace fszm {

#define OPSSL_CHECK_PARAM(p) if (p == nullptr) { return -1; }

// AI generate
class openssl_utils {
public:
	// md5
	static std::string md5(const std::string& str) {
		return md5((unsigned char*) str.c_str(), str.size());
	}
	static std::string md5(unsigned char* data, size_t data_len, bool upper = true) {
		return evp_digest_op(EVP_md5(), data, data_len, upper);
	}

	// sha1
	static std::string sha1(const std::string& str) {
		return sha1((unsigned char*) str.c_str(), str.size());
	}
	static std::string sha1(unsigned char* data, size_t data_len, bool upper = true) {
		return evp_digest_op(EVP_sha1(), data, data_len, upper);
	}

	// sha2
	static std::string sha224(unsigned char* data, size_t data_len, bool upper = true) {
		return evp_digest_op(EVP_sha224(), data, data_len, upper);
	}
	static std::string sha256(unsigned char* data, size_t data_len, bool upper = true) {
		return evp_digest_op(EVP_sha256(), data, data_len, upper);
	}
	static std::string sha384(unsigned char* data, size_t data_len, bool upper = true) {
		return evp_digest_op(EVP_sha384(), data, data_len, upper);
	}
	static std::string sha512(unsigned char* data, size_t data_len, bool upper = true) {
		return evp_digest_op(EVP_sha512(), data, data_len, upper);
	}

	// ripemd160
	static std::string ripemd160(unsigned char* data, size_t data_len, bool upper = true) {
		return evp_digest_op(EVP_ripemd160(), data, data_len, upper);
	}

	// base64
	static char* base64Encode(const char* buffer, int length, int& outLen, bool newLine = false) {
		if (!buffer) {
			outLen = 0;
			return nullptr;
		}
	    BIO* bmem = nullptr;
	    BIO* b64 = nullptr;
	    BUF_MEM* bptr;

	    b64 = BIO_new(BIO_f_base64());
	    if (!newLine) {
	        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
	    }
	    bmem = BIO_new(BIO_s_mem());
	    b64 = BIO_push(b64, bmem);
	    BIO_write(b64, buffer, length);
	    BIO_flush(b64);
	    BIO_get_mem_ptr(b64, &bptr);
	    BIO_set_close(b64, BIO_NOCLOSE);

	    char* buff = (char*) malloc(bptr->length + 1);
	    memcpy(buff, bptr->data, bptr->length);
	    buff[bptr->length] = 0;
	    outLen = bptr->length;
	    BIO_free_all(b64);

	    return buff;
	}

	static char* base64Decode(const char* input, int length, int& outLen, bool newLine = false) {
		if (!input) {
			outLen = 0;
			return nullptr;
		}
	    BIO* bmem = nullptr;
	    BIO* b64 = nullptr;

	    char* buffer = (char*) malloc(length+1);
	    memset(buffer, 0, length+1);
	    b64 = BIO_new(BIO_f_base64());
	    if (!newLine) {
	        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
	    }
	    bmem = BIO_new_mem_buf(input, length);
	    bmem = BIO_push(b64, bmem);
	    outLen = BIO_read(bmem, buffer, length);
	    BIO_free_all(bmem);

	    return buffer;
	}

	static std::string base64_encode(const std::string& input) {
		if (input.empty()) return "";

		// 3 byte -> 4 char
		int out_len = EVP_ENCODE_LENGTH(input.length());
		std::string out(out_len, '\0');

		int len = EVP_EncodeBlock(
			reinterpret_cast<unsigned char*>(&out[0]),
			reinterpret_cast<const unsigned char*>(input.data()),
			static_cast<int>(input.length())
		);		
		out.resize(len);

		// remove last '\n' if length == (48/80) * N
		if (!out.empty() && out.back() == '\n') {
			out.pop_back();
		}
		return out;
	}

	static std::string base64_decode(const std::string& input) {
		if (input.empty()) return "";

		// stat tail's '=' padding, which will occur 0x00
		int padding = 0;
		if (!input.empty()) {
			if (input.back() == '=') padding++;
			if (input.size() >= 2 && input[input.size() - 2] == '=') padding++;
		}

		// 4 char -> 3 byte
		int out_len = EVP_DECODE_LENGTH(input.length());
		std::string out(out_len, '\0');

		int len = EVP_DecodeBlock(
			reinterpret_cast<unsigned char*>(&out[0]),
			reinterpret_cast<const unsigned char*>(input.data()),
			static_cast<int>(input.length())
		);
		// remove extra padding(0x00) due to '='
		// printf("len=%d, pad=%d\n", len, padding);
		out.resize(len - padding);
		return out;
	}

	// des-ecb, padding for zero
	static int des_encrypt_ecb(unsigned char* in_data, unsigned int in_len, unsigned char* in_key, bool encrypt,
		unsigned char** out_data, unsigned int* out_len) {
		OPSSL_CHECK_PARAM(in_data);
		OPSSL_CHECK_PARAM(in_key);
		return evp_cipher_op(EVP_des_ecb(), in_data, in_len, in_key, nullptr, encrypt, out_data, out_len);
	}

	// des-cbc
	static int des_encrypt_cbc(unsigned char* in_data, unsigned int in_len, unsigned char* in_key, unsigned char* in_ivec, bool encrypt,
		unsigned char** out_data, unsigned int* out_len) {
		OPSSL_CHECK_PARAM(in_data);
		OPSSL_CHECK_PARAM(in_key);
		OPSSL_CHECK_PARAM(in_ivec);
		return evp_cipher_op(EVP_des_cbc(), in_data, in_len, in_key, in_ivec, encrypt, out_data, out_len);
	}

	// des-cfb
	static int des_encrypt_cfb(unsigned char* in_data, unsigned int in_len, unsigned char* in_key, unsigned char* in_ivec, bool encrypt,
		unsigned char** out_data, unsigned int* out_len) {
		OPSSL_CHECK_PARAM(in_data);
		OPSSL_CHECK_PARAM(in_key);
		OPSSL_CHECK_PARAM(in_ivec);
		return evp_cipher_op(EVP_des_cfb64(), in_data, in_len, in_key, in_ivec, encrypt, out_data, out_len);
	}

	// des-ofb
	static int des_encrypt_ofb(unsigned char* in_data, unsigned int in_len, unsigned char* in_key, unsigned char* in_ivec,
		unsigned char** out_data, unsigned int* out_len) {
		OPSSL_CHECK_PARAM(in_data);
		OPSSL_CHECK_PARAM(in_key);
		OPSSL_CHECK_PARAM(in_ivec);
		return evp_cipher_op(EVP_des_ofb(), in_data, in_len, in_key, in_ivec, true, out_data, out_len);
	}

	// 3des-ecb, padding for zero
	static int des_encrypt_ecb3(unsigned char* in_data, unsigned int in_len, unsigned char* in_key, bool encrypt,
		unsigned char** out_data, unsigned int* out_len) {
		OPSSL_CHECK_PARAM(in_data);
		OPSSL_CHECK_PARAM(in_key);
		return evp_cipher_op(EVP_des_ede3_ecb(), in_data, in_len, in_key, nullptr, encrypt, out_data, out_len);
	}

	// 3des-cbc
	static int des_encrypt_cbc3(unsigned char* in_data, unsigned int in_len, unsigned char* in_key, unsigned char* in_ivec, bool encrypt,
		unsigned char** out_data, unsigned int* out_len) {
		OPSSL_CHECK_PARAM(in_data);
		OPSSL_CHECK_PARAM(in_key);
		OPSSL_CHECK_PARAM(in_ivec);
		return evp_cipher_op(EVP_des_ede3_cbc(), in_data, in_len, in_key, in_ivec, encrypt, out_data, out_len);
	}

	// 3des-cfb
	static int des_encrypt_cfb3(unsigned char* in_data, unsigned int in_len, unsigned char* in_key, unsigned char* in_ivec, bool encrypt,
		unsigned char** out_data, unsigned int* out_len) {
		OPSSL_CHECK_PARAM(in_data);
		OPSSL_CHECK_PARAM(in_key);
		OPSSL_CHECK_PARAM(in_ivec);
		return evp_cipher_op(EVP_des_ede3_cfb64(), in_data, in_len, in_key, in_ivec, encrypt, out_data, out_len);
	}

	// 3des-ofb
	static int des_encrypt_ofb3(unsigned char* in_data, unsigned int in_len, unsigned char* in_key, unsigned char* in_ivec,
		unsigned char** out_data, unsigned int* out_len) {
		OPSSL_CHECK_PARAM(in_data);
		OPSSL_CHECK_PARAM(in_key);
		OPSSL_CHECK_PARAM(in_ivec);
		return evp_cipher_op(EVP_des_ede3_ofb(), in_data, in_len, in_key, in_ivec, true, out_data, out_len);
	}

	// camellia-ecb, bits: 128/192/256
	static int camellia_encrypt_ecb(unsigned char* in_data, unsigned int in_len, unsigned char* in_key, int bits, bool encrypt,
		unsigned char** out_data, unsigned int* out_len) {
		OPSSL_CHECK_PARAM(in_data);
		OPSSL_CHECK_PARAM(in_key);
		const EVP_CIPHER* cipher = nullptr;
		switch (bits) {
			case 128: cipher = EVP_camellia_128_ecb(); break;
			case 192: cipher = EVP_camellia_192_ecb(); break;
			case 256: cipher = EVP_camellia_256_ecb(); break;
			default: return -1;
		}
		return evp_cipher_op(cipher, in_data, in_len, in_key, nullptr, encrypt, out_data, out_len);
	}

	// camellia-cbc
	static int camellia_encrypt_cbc(unsigned char* in_data, unsigned int in_len, unsigned char* in_key, int bits, unsigned char* in_ivec, bool encrypt,
		unsigned char** out_data, unsigned int* out_len) {
		OPSSL_CHECK_PARAM(in_data);
		OPSSL_CHECK_PARAM(in_key);
		OPSSL_CHECK_PARAM(in_ivec);
		const EVP_CIPHER* cipher = nullptr;
		switch (bits) {
			case 128: cipher = EVP_camellia_128_cbc(); break;
			case 192: cipher = EVP_camellia_192_cbc(); break;
			case 256: cipher = EVP_camellia_256_cbc(); break;
			default: return -1;
		}
		return evp_cipher_op(cipher, in_data, in_len, in_key, in_ivec, encrypt, out_data, out_len);
	}

	// camellia-cfb
	static int camellia_encrypt_cfb(unsigned char* in_data, unsigned int in_len, unsigned char* in_key, int bits, unsigned char* in_ivec, bool encrypt,
		unsigned char** out_data, unsigned int* out_len) {
		OPSSL_CHECK_PARAM(in_data);
		OPSSL_CHECK_PARAM(in_key);
		OPSSL_CHECK_PARAM(in_ivec);
		const EVP_CIPHER* cipher = nullptr;
		switch (bits) {
			case 128: cipher = EVP_camellia_128_cfb128(); break;
			case 192: cipher = EVP_camellia_192_cfb128(); break;
			case 256: cipher = EVP_camellia_256_cfb128(); break;
			default: return -1;
		}
		return evp_cipher_op(cipher, in_data, in_len, in_key, in_ivec, encrypt, out_data, out_len);
	}

	// camellia-ofb
	static int camellia_encrypt_ofb(unsigned char* in_data, unsigned int in_len, unsigned char* in_key, int bits, unsigned char* in_ivec,
		unsigned char** out_data, unsigned int* out_len) {
		OPSSL_CHECK_PARAM(in_data);
		OPSSL_CHECK_PARAM(in_key);
		OPSSL_CHECK_PARAM(in_ivec);
		const EVP_CIPHER* cipher = nullptr;
		switch (bits) {
			case 128: cipher = EVP_camellia_128_ofb(); break;
			case 192: cipher = EVP_camellia_192_ofb(); break;
			case 256: cipher = EVP_camellia_256_ofb(); break;
			default: return -1;
		}
		return evp_cipher_op(cipher, in_data, in_len, in_key, in_ivec, true, out_data, out_len);
	}

	// camellia-ctr
	static int camellia_encrypt_ctr(unsigned char* in_data, unsigned int in_len, unsigned char* in_key, int bits, unsigned char* in_ivec,
		unsigned char** out_data, unsigned int* out_len) {
		OPSSL_CHECK_PARAM(in_data);
		OPSSL_CHECK_PARAM(in_key);
		OPSSL_CHECK_PARAM(in_ivec);
		const EVP_CIPHER* cipher = nullptr;
		switch (bits) {
			case 128: cipher = EVP_camellia_128_ctr(); break;
			case 192: cipher = EVP_camellia_192_ctr(); break;
			case 256: cipher = EVP_camellia_256_ctr(); break;
			default: return -1;
		}
		return evp_cipher_op(cipher, in_data, in_len, in_key, in_ivec, true, out_data, out_len);
	}

	// aes-ecb, bits: 128/192/256
	static int aes_encrypt_ecb(unsigned char* in_data, unsigned int in_len, unsigned char* in_key, int bits, bool encrypt,
		unsigned char** out_data, unsigned int* out_len) {
		OPSSL_CHECK_PARAM(in_data);
		OPSSL_CHECK_PARAM(in_key);
		const EVP_CIPHER* cipher = nullptr;
		switch (bits) {
			case 128: cipher = EVP_aes_128_ecb(); break;
			case 192: cipher = EVP_aes_192_ecb(); break;
			case 256: cipher = EVP_aes_256_ecb(); break;
			default: return -1;
		}
		return evp_cipher_op(cipher, in_data, in_len, in_key, nullptr, encrypt, out_data, out_len);
	}

	// aes-cbc
	static int aes_encrypt_cbc(unsigned char* in_data, unsigned int in_len, unsigned char* in_key, int bits, unsigned char* in_ivec, bool encrypt,
		unsigned char** out_data, unsigned int* out_len) {
		OPSSL_CHECK_PARAM(in_data);
		OPSSL_CHECK_PARAM(in_key);
		OPSSL_CHECK_PARAM(in_ivec);
		const EVP_CIPHER* cipher = nullptr;
		switch (bits) {
			case 128: cipher = EVP_aes_128_cbc(); break;
			case 192: cipher = EVP_aes_192_cbc(); break;
			case 256: cipher = EVP_aes_256_cbc(); break;
			default: return -1;
		}
		return evp_cipher_op(cipher, in_data, in_len, in_key, in_ivec, encrypt, out_data, out_len);
	}

	// aes-cfb
	static int aes_encrypt_cfb(unsigned char* in_data, unsigned int in_len, unsigned char* in_key, int bits, unsigned char* in_ivec, bool encrypt,
		unsigned char** out_data, unsigned int* out_len) {
		OPSSL_CHECK_PARAM(in_data);
		OPSSL_CHECK_PARAM(in_key);
		OPSSL_CHECK_PARAM(in_ivec);
		const EVP_CIPHER* cipher = nullptr;
		switch (bits) {
			case 128: cipher = EVP_aes_128_cfb128(); break;
			case 192: cipher = EVP_aes_192_cfb128(); break;
			case 256: cipher = EVP_aes_256_cfb128(); break;
			default: return -1;
		}
		return evp_cipher_op(cipher, in_data, in_len, in_key, in_ivec, encrypt, out_data, out_len);
	}

	// aes-ofb
	static int aes_encrypt_ofb(unsigned char* in_data, unsigned int in_len, unsigned char* in_key, int bits, unsigned char* in_ivec,
		unsigned char** out_data, unsigned int* out_len) {
		OPSSL_CHECK_PARAM(in_data);
		OPSSL_CHECK_PARAM(in_key);
		OPSSL_CHECK_PARAM(in_ivec);
		const EVP_CIPHER* cipher = nullptr;
		switch (bits) {
			case 128: cipher = EVP_aes_128_ofb(); break;
			case 192: cipher = EVP_aes_192_ofb(); break;
			case 256: cipher = EVP_aes_256_ofb(); break;
			default: return -1;
		}
		return evp_cipher_op(cipher, in_data, in_len, in_key, in_ivec, true, out_data, out_len);
	}

	// aes-ctr
	static int aes_encrypt_ctr(unsigned char* in_data, unsigned int in_len, unsigned char* in_key, int bits, unsigned char* in_ivec,
		unsigned char** out_data, unsigned int* out_len) {
		OPSSL_CHECK_PARAM(in_data);
		OPSSL_CHECK_PARAM(in_key);
		OPSSL_CHECK_PARAM(in_ivec);
		const EVP_CIPHER* cipher = nullptr;
		switch (bits) {
			case 128: cipher = EVP_aes_128_ctr(); break;
			case 192: cipher = EVP_aes_192_ctr(); break;
			case 256: cipher = EVP_aes_256_ctr(); break;
			default: return -1;
		}
		return evp_cipher_op(cipher, in_data, in_len, in_key, in_ivec, true, out_data, out_len);
	}

	enum eEncryptType {
		eET_DES_ECB,
		eET_DES_CBC,
		eET_DES_CFB,
		eET_DES_OFB,

		eET_3DES_ECB,
		eET_3DES_CBC,
		eET_3DES_CFB,
		eET_3DES_OFB,
		
		eET_CAMELLIA_ECB,
		eET_CAMELLIA_CBC,
		eET_CAMELLIA_CFB,
		eET_CAMELLIA_OFB,
		eET_CAMELLIA_CTR,

		eET_AES_ECB,
		eET_AES_CBC,
		eET_AES_CFB,
		eET_AES_OFB,
		eET_AES_CTR,

		eET_RSA_PUBLIC_ENC,
		eET_RSA_PRIVATE_DEC,		
		eET_RSA_PRIVATE_ENC,
		eET_RSA_PUBLIC_DEC,
	};

	// no include ede-xxx(2-des) algorithm
	static int symmetric_encrypt(const std::string& in_str, const std::string& in_key, 
		const std::string& in_ivec, eEncryptType eType, bool encrypt, std::string& out_str) {
		const int bits = 128; // 128:16-char | 192:24-char | 256:32-char
		unsigned char* out_data = 0;
		unsigned int out_len = 0;
		int rc = -1;

		if (in_key.size() < 8 || in_ivec.size() < 8) return -1;
		if ((eType == eET_3DES_ECB || eType == eET_3DES_CBC) && in_key.size() < 3*8) return -1;

		switch (eType) {
		
		// AES
		// when bits=128,key=16 ivec=16; when bits=192,key=24 ivec=16; when bits=256,key=32 ivec=16
		case eET_AES_ECB:
			rc = aes_encrypt_ecb((unsigned char*) in_str.c_str(), (unsigned int) in_str.length(), 
				(unsigned char*) in_key.c_str(), bits, encrypt, &out_data, &out_len);
			break;
		case eET_AES_CBC:
			rc = aes_encrypt_cbc((unsigned char*) in_str.c_str(), (unsigned int) in_str.length(), 
				(unsigned char*) in_key.c_str(), bits, (unsigned char*) in_ivec.c_str(), encrypt, &out_data, &out_len);
			break;
		case eET_AES_CFB:
			rc = aes_encrypt_cfb((unsigned char*) in_str.c_str(), (unsigned int) in_str.length(), 
				(unsigned char*) in_key.c_str(), bits, (unsigned char*) in_ivec.c_str(), encrypt, &out_data, &out_len);
			break;
		case eET_AES_OFB:
			rc = aes_encrypt_ofb((unsigned char*) in_str.c_str(), (unsigned int) in_str.length(), 
				(unsigned char*) in_key.c_str(), bits, (unsigned char*) in_ivec.c_str(), &out_data, &out_len);
			break;
		case eET_AES_CTR:
			rc = aes_encrypt_ctr((unsigned char*) in_str.c_str(), (unsigned int) in_str.length(), 
				(unsigned char*) in_key.c_str(), bits, (unsigned char*) in_ivec.c_str(), &out_data, &out_len);
			break;
		
		// Camellia
		// when bits=128,key=16 ivec=16; when bits=192,key=24 ivec=16; when bits=256,key=32 ivec=16
		case eET_CAMELLIA_ECB:
			rc = camellia_encrypt_ecb((unsigned char*) in_str.c_str(), (unsigned int) in_str.length(), 
				(unsigned char*) in_key.c_str(), bits, encrypt, &out_data, &out_len);
			break;
		case eET_CAMELLIA_CBC:
			rc = camellia_encrypt_cbc((unsigned char*) in_str.c_str(), (unsigned int) in_str.length(), 
				(unsigned char*) in_key.c_str(), bits, (unsigned char*) in_ivec.c_str(), encrypt, &out_data, &out_len);
			break;
		case eET_CAMELLIA_CFB:
			rc = camellia_encrypt_cfb((unsigned char*) in_str.c_str(), (unsigned int) in_str.length(), 
				(unsigned char*) in_key.c_str(), bits, (unsigned char*) in_ivec.c_str(), encrypt, &out_data, &out_len);
			break;
		case eET_CAMELLIA_OFB:
			rc = camellia_encrypt_ofb((unsigned char*) in_str.c_str(), (unsigned int) in_str.length(), 
				(unsigned char*) in_key.c_str(), bits, (unsigned char*) in_ivec.c_str(), &out_data, &out_len);
			break;
		case eET_CAMELLIA_CTR:
			rc = camellia_encrypt_ctr((unsigned char*) in_str.c_str(), (unsigned int) in_str.length(), 
				(unsigned char*) in_key.c_str(), bits, (unsigned char*) in_ivec.c_str(), &out_data, &out_len);
			break;

		// 3DES, key:192bit/24char, ivec:64bit/8char
		case eET_3DES_ECB:
			rc = des_encrypt_ecb3((unsigned char*) in_str.c_str(), (unsigned int) in_str.length(), 
				(unsigned char*) in_key.c_str(), encrypt, &out_data, &out_len);
			break;
		case eET_3DES_CBC:
			rc = des_encrypt_cbc3((unsigned char*) in_str.c_str(), (unsigned int) in_str.length(), 
				(unsigned char*) in_key.c_str(), (unsigned char*) in_ivec.c_str(), encrypt, &out_data, &out_len);
			break;
		case eET_3DES_CFB:
			rc = des_encrypt_cfb3((unsigned char*) in_str.c_str(), (unsigned int) in_str.length(), 
				(unsigned char*) in_key.c_str(), (unsigned char*) in_ivec.c_str(), encrypt, &out_data, &out_len);
			break;
		case eET_3DES_OFB:
			rc = des_encrypt_ofb3((unsigned char*) in_str.c_str(), (unsigned int) in_str.length(), 
				(unsigned char*) in_key.c_str(), (unsigned char*) in_ivec.c_str(), &out_data, &out_len);
			break;
		
		// DES, key:64bit/8char, ivec:64bit/8char
		case eET_DES_ECB:
			rc = des_encrypt_ecb((unsigned char*) in_str.c_str(), (unsigned int) in_str.length(), 
				(unsigned char*) in_key.c_str(), encrypt, &out_data, &out_len);
			break;
		case eET_DES_CBC:
			rc = des_encrypt_cbc((unsigned char*) in_str.c_str(), (unsigned int) in_str.length(), 
				(unsigned char*) in_key.c_str(), (unsigned char*) in_ivec.c_str(), encrypt, &out_data, &out_len);
			break;
		case eET_DES_CFB:
			rc = des_encrypt_cfb((unsigned char*) in_str.c_str(), (unsigned int) in_str.length(), 
				(unsigned char*) in_key.c_str(), (unsigned char*) in_ivec.c_str(), encrypt, &out_data, &out_len);
			break;
		case eET_DES_OFB:
			rc = des_encrypt_ofb((unsigned char*) in_str.c_str(), (unsigned int) in_str.length(), 
				(unsigned char*) in_key.c_str(), (unsigned char*) in_ivec.c_str(), &out_data, &out_len);
			break;
		
		default:
			break;
		}

		if (!rc) { // success
			out_str = std::string((char*) out_data, out_len);
			free((void*) out_data);
		}
		return rc;
	}

	static EVP_PKEY* load_rsa_key(const char* file_path, bool public_key) {
		return load_pkey(file_path, public_key);
	}
	static void free_rsa_key(EVP_PKEY** key) {
		free_pkey(key);
	}

	// return transfered size
	static int rsa_encrypt(unsigned char* in_data, unsigned int in_len, EVP_PKEY* in_key, eEncryptType eType,
		unsigned char** out_data, unsigned int* out_len) {
		OPSSL_CHECK_PARAM(in_data);
		OPSSL_CHECK_PARAM(in_key);
		OPSSL_CHECK_PARAM(out_data);
		OPSSL_CHECK_PARAM(out_len);
		*out_data = nullptr;
		EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(in_key, nullptr);
		if (!ctx) return -1;

		int rc = -1;
		switch (eType) {
			case eET_RSA_PUBLIC_ENC:
				if (EVP_PKEY_encrypt_init(ctx) <= 0) break;
				EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING);
				rc = pkey_op(ctx, EVP_PKEY_encrypt, in_data, in_len, out_data, out_len);
				break;
			case eET_RSA_PRIVATE_DEC:
				if (EVP_PKEY_decrypt_init(ctx) <= 0) break;
				EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING);
				rc = pkey_op(ctx, EVP_PKEY_decrypt, in_data, in_len, out_data, out_len);
				break;
			case eET_RSA_PRIVATE_ENC:
				if (EVP_PKEY_sign_init(ctx) <= 0) break;
				EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING);
				rc = pkey_op(ctx, EVP_PKEY_sign, in_data, in_len, out_data, out_len);
				break;
			case eET_RSA_PUBLIC_DEC:
				if (EVP_PKEY_verify_recover_init(ctx) <= 0) break;
				EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING);
				rc = pkey_op(ctx, EVP_PKEY_verify_recover, in_data, in_len, out_data, out_len);
				break;
			default:
				break;
		}

		EVP_PKEY_CTX_free(ctx);
		if (rc < 0) {
			free(*out_data);
			*out_data = nullptr;
			*out_len = 0;
		}
		return rc;
	}

	static EVP_PKEY* load_dsa_key(const char* file_path, bool public_key) {
		return load_pkey(file_path, public_key);
	}
	static void free_dsa_key(EVP_PKEY** key) {
		free_pkey(key);
	}

	static int dsa_sign(unsigned char* dgst, int dlen, EVP_PKEY* dsa_pri, unsigned char** sig, unsigned int* siglen) {
		return pkey_sign(dgst, dlen, dsa_pri, sig, siglen);
	}

	static int dsa_verify(unsigned char* dgst, int dlen, EVP_PKEY* dsa_pub, const unsigned char* sig, int siglen) {
		return pkey_verify(dgst, dlen, dsa_pub, sig, siglen);
	}

	// DH-Exchange
	// Generate DH keypair with random prime (for initiator)
	static EVP_PKEY* dh_generate(int prime_len = 512) {
		EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_DH, nullptr);
		if (!pctx) return nullptr;
		if (EVP_PKEY_paramgen_init(pctx) <= 0 ||
		    EVP_PKEY_CTX_set_dh_paramgen_prime_len(pctx, prime_len) <= 0 ||
		    EVP_PKEY_CTX_set_dh_paramgen_generator(pctx, DH_GENERATOR_2) <= 0) {
			EVP_PKEY_CTX_free(pctx);
			return nullptr;
		}
		EVP_PKEY* params = nullptr;
		if (EVP_PKEY_paramgen(pctx, &params) <= 0) {
			EVP_PKEY_CTX_free(pctx);
			return nullptr;
		}
		EVP_PKEY_CTX_free(pctx);

		EVP_PKEY_CTX* ck_ctx = EVP_PKEY_CTX_new(params, nullptr);
		if (!ck_ctx || EVP_PKEY_param_check(ck_ctx) != 1) {
			EVP_PKEY_CTX_free(ck_ctx);
			EVP_PKEY_free(params);
			return nullptr;
		}
		EVP_PKEY_CTX_free(ck_ctx);

		EVP_PKEY* key = dh_keygen_from_params(params);
		EVP_PKEY_free(params);
		return key;
	}

	// Generate DH keypair from peer's raw p,g bytes (for responder)
	static EVP_PKEY* dh_generate(const std::string& p, const std::string& g) {
		if (p.empty() || g.empty()) return nullptr;
		BIGNUM* bn_p = BN_bin2bn((const unsigned char*)p.data(), p.size(), nullptr);
		BIGNUM* bn_g = BN_bin2bn((const unsigned char*)g.data(), g.size(), nullptr);
		if (!bn_p || !bn_g) {
			BN_free(bn_p);
			BN_free(bn_g);
			return nullptr;
		}
		EVP_PKEY* params = dh_create_params(bn_p, bn_g);
		BN_free(bn_p);
		BN_free(bn_g);
		if (!params) return nullptr;

		EVP_PKEY* key = dh_keygen_from_params(params);
		EVP_PKEY_free(params);
		return key;
	}

	static void dh_destroy(EVP_PKEY** dh) {
		free_pkey(dh);
	}

	// Export DH parameter as raw bytes (big-endian).
	// name: "p", "g", "pub"
	static std::string dh_export(EVP_PKEY* key, const char* name) {
		if (!key || !name) return "";
		BIGNUM* bn = nullptr;
		if (!EVP_PKEY_get_bn_param(key, name, &bn)) return "";
		int len = BN_num_bytes(bn);
		std::string out(len, '\0');
		BN_bn2bin(bn, (unsigned char*)&out[0]);
		BN_free(bn);
		return out;
	}

	// Compute DH shared secret.
	// peer_pub: raw public key bytes exported by dh_export(key, "pub") from the peer.
	// return sharekey length if success, -1 on fail
	static int dh_compute_sharekey(EVP_PKEY* dh, const std::string& peer_pub, unsigned char* sharekey, int len) {
		OPSSL_CHECK_PARAM(dh);
		OPSSL_CHECK_PARAM(sharekey);
		if (peer_pub.empty()) return -1;

		BIGNUM* p = nullptr;
		BIGNUM* g = nullptr;
		if (!EVP_PKEY_get_bn_param(dh, "p", &p) || !EVP_PKEY_get_bn_param(dh, "g", &g)) {
			BN_free(p);
			BN_free(g);
			return -1;
		}

		BIGNUM* pub = BN_bin2bn((const unsigned char*)peer_pub.data(), peer_pub.size(), nullptr);
		if (!pub) {
			BN_free(p);
			BN_free(g);
			return -1;
		}

		EVP_PKEY* peer = dh_build_from_bn(p, g, pub);
		BN_free(p);
		BN_free(g);
		BN_free(pub);

		int rc = -1;
		if (peer) {
			rc = pkey_derive_shared(dh, peer, sharekey, len);
			EVP_PKEY_free(peer);
		}
		return rc;
	}

	// ECC Exchange
	// Create ECC key pair by curve NID (e.g. NID_X9_62_prime256v1).
	// Use OBJ_sn2nid("prime256v1") or EC_curve_nid2nist() to discover NIDs.
	// 1 on success and -1 on fail
	static int ecc_create(int curve_nid, EVP_PKEY** key_out) {
		OPSSL_CHECK_PARAM(key_out);
		*key_out = nullptr;

		EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
		if (!ctx) return -1;
		if (EVP_PKEY_keygen_init(ctx) <= 0 || EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, curve_nid) <= 0) {
			EVP_PKEY_CTX_free(ctx);
			return -1;
		}

		EVP_PKEY* key = nullptr;
		if (EVP_PKEY_keygen(ctx, &key) <= 0) {
			EVP_PKEY_CTX_free(ctx);
			return -1;
		}
		EVP_PKEY_CTX_free(ctx);
		*key_out = key;
		return 1;
	}

	static void ecc_destroy(EVP_PKEY** key) {
		free_pkey(key);
	}

	// Export public key as raw bytes (uncompressed point format).
	// This is the natural format for network transmission.
	static std::string ecc_export_pubkey(EVP_PKEY* key) {
		if (!key) return "";
		size_t pub_len = 0;
		if (!EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, nullptr, 0, &pub_len))
			return "";
		std::string out(pub_len, '\0');
		if (!EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, (unsigned char*)&out[0], pub_len, &pub_len))
			return "";
		out.resize(pub_len);
		return out;
	}

	// Import public key from raw bytes (exported by ecc_export_pubkey) to EVP_PKEY.
	// curve_nid must match the curve used to generate the original key.
	// Caller must free with ecc_destroy().
	static EVP_PKEY* ecc_import_pubkey(int curve_nid, const std::string& pub) {
		if (pub.empty()) return nullptr;
		const char* curve_name = OBJ_nid2sn(curve_nid);
		if (!curve_name) return nullptr;

		OSSL_PARAM params[3];
		params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, (char*)curve_name, 0);
		params[1] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, (void*)pub.data(), pub.size());
		params[2] = OSSL_PARAM_construct_end();

		EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
		if (!pctx) return nullptr;

		EVP_PKEY* pub_key = nullptr;
		if (EVP_PKEY_fromdata_init(pctx) <= 0 ||
		    EVP_PKEY_fromdata(pctx, &pub_key, OSSL_KEYMGMT_SELECT_DOMAIN_PARAMETERS | OSSL_KEYMGMT_SELECT_PUBLIC_KEY, params) <= 0) {
			EVP_PKEY_CTX_free(pctx);
			return nullptr;
		}
		EVP_PKEY_CTX_free(pctx);
		return pub_key;
	}

	// Compute ECDH shared secret.
	// peer_pub: raw public key bytes exported by ecc_export_pubkey() from the peer.
	// curve_nid: must match the curve used to generate the keys.
	// return sharekey length if success, -1 on fail
	static int ecc_compute_sharekey(EVP_PKEY* key, int curve_nid, const std::string& peer_pub, unsigned char* sharekey, int len) {
		OPSSL_CHECK_PARAM(key);
		OPSSL_CHECK_PARAM(sharekey);
		if (peer_pub.empty()) return -1;

		EVP_PKEY* peer = ecc_import_pubkey(curve_nid, peer_pub);
		if (!peer) return -1;

		int rc = pkey_derive_shared(key, peer, sharekey, len);
		EVP_PKEY_free(peer);
		return rc;
	}

	static int ecc_sign(unsigned char* dgst, int dlen, EVP_PKEY* key, unsigned char** sig, unsigned int* siglen) {
		return pkey_sign(dgst, dlen, key, sig, siglen);
	}

	static int ecc_verify(unsigned char* dgst, int dlen, EVP_PKEY* key, const unsigned char* sig, int siglen) {
		return pkey_verify(dgst, dlen, key, sig, siglen);
	}

public:
	static std::string hex2string(unsigned char* data, size_t data_len, bool upper = true) {
		if (!data) return "";
		std::string result;
		char sep[3] = { 0 };
		const char* fmt = upper ? "%02X" : "%02x";
		for (size_t i = 0; i < data_len; ++i) {
		    sprintf(sep, fmt, data[i]);
		    result.append(sep);
		}
		return result;
	}

	static unsigned char* alloc_memory(unsigned int size) {
		unsigned char* mem = (unsigned char*) malloc(size);
		if (mem) memset(mem, 0, size);
		return mem;
	}

private:
	static std::string evp_digest_op(const EVP_MD* md, unsigned char* data, size_t data_len, bool upper = true) {
		if (!data) return "";
		unsigned char digest[EVP_MAX_MD_SIZE];
		unsigned int digest_len = 0;
		EVP_MD_CTX* ctx = EVP_MD_CTX_new();
		EVP_DigestInit_ex(ctx, md, nullptr);
		EVP_DigestUpdate(ctx, data, data_len);
		EVP_DigestFinal_ex(ctx, digest, &digest_len);
		EVP_MD_CTX_free(ctx);
		return hex2string(digest, digest_len, upper);
	}

	static int evp_cipher_op(const EVP_CIPHER* cipher, unsigned char* in_data, unsigned int in_len,
		unsigned char* in_key, unsigned char* in_ivec, bool encrypt, unsigned char** out_data, unsigned int* out_len) {
		OPSSL_CHECK_PARAM(out_data);
		OPSSL_CHECK_PARAM(out_len);
		int block_size = EVP_CIPHER_block_size(cipher);
		bool is_block = block_size > 1;

		if (is_block) {
			unsigned int times = (in_len + block_size - 1) / block_size;
			*out_len = times * block_size;
		} else {
			*out_len = in_len;
		}

		*out_data = alloc_memory(*out_len);
		if (!*out_data) return -100;

		unsigned char* input = in_data;
		unsigned char* padded = nullptr;
		if (is_block && *out_len > in_len) {
			padded = alloc_memory(*out_len);
			if (!padded) {
				free(*out_data);
				return -100;
			}
			memcpy(padded, in_data, in_len);
			input = padded;
		}

		EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
		if (!ctx) {
			free(padded);
			free(*out_data);
			return -100;
		}

		int outl = 0;
		int tmpl = 0;
		int rc = 0;
		if (encrypt)
			rc = EVP_EncryptInit_ex(ctx, cipher, nullptr, in_key, in_ivec);
		else
			rc = EVP_DecryptInit_ex(ctx, cipher, nullptr, in_key, in_ivec);

		if (rc != 1) {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
			OSSL_PROVIDER_load(NULL, "legacy");
			EVP_CIPHER_CTX_free(ctx);
			ctx = EVP_CIPHER_CTX_new();
			if (!ctx) {
				free(padded);
				free(*out_data);
				return -100;
			}
			if (encrypt)
				rc = EVP_EncryptInit_ex(ctx, cipher, nullptr, in_key, in_ivec);
			else
				rc = EVP_DecryptInit_ex(ctx, cipher, nullptr, in_key, in_ivec);
#endif
			if (rc != 1) goto EXIT;
		}

		if (is_block) EVP_CIPHER_CTX_set_padding(ctx, 0);

		if (encrypt)
			rc = EVP_EncryptUpdate(ctx, *out_data, &outl, input, *out_len);
		else
			rc = EVP_DecryptUpdate(ctx, *out_data, &outl, input, *out_len);
		if (rc != 1) goto EXIT;

		if (encrypt)
			rc = EVP_EncryptFinal_ex(ctx, *out_data + outl, &tmpl);
		else
			rc = EVP_DecryptFinal_ex(ctx, *out_data + outl, &tmpl);

	EXIT:
		EVP_CIPHER_CTX_free(ctx);
		free(padded);
		if (rc != 1) {
			free(*out_data);
			return -1;
		}
		return 0;
	}

	static int pkey_op(EVP_PKEY_CTX* ctx, int (*op)(EVP_PKEY_CTX*, unsigned char*, size_t*, const unsigned char*, size_t),
		unsigned char* in_data, unsigned int in_len, unsigned char** out_data, unsigned int* out_len) {
		size_t outlen = 0;
		if (op(ctx, nullptr, &outlen, in_data, in_len) <= 0) return -1;
		*out_data = alloc_memory((unsigned int)outlen);
		if (!*out_data) return -100;
		if (op(ctx, *out_data, &outlen, in_data, in_len) <= 0) {
			free(*out_data);
			*out_data = nullptr;
			return -1;
		}
		*out_len = (unsigned int)outlen;
		return (int)outlen;
	}

	static int pkey_derive_shared(EVP_PKEY* own_key, EVP_PKEY* peer_key, unsigned char* out, int len) {
		OPSSL_CHECK_PARAM(own_key);
		OPSSL_CHECK_PARAM(peer_key);
		OPSSL_CHECK_PARAM(out);

		EVP_PKEY_CTX* dctx = EVP_PKEY_CTX_new(own_key, nullptr);
		if (!dctx) return -1;

		size_t outlen = 0;
		int rc = -1;
		if (EVP_PKEY_derive_init(dctx) > 0 &&
		    EVP_PKEY_derive_set_peer(dctx, peer_key) > 0 &&
		    EVP_PKEY_derive(dctx, nullptr, &outlen) > 0 &&
		    outlen <= (size_t)len &&
		    EVP_PKEY_derive(dctx, out, &outlen) > 0) {
			rc = (int)outlen;
		}

		EVP_PKEY_CTX_free(dctx);
		return rc;
	}

	static EVP_PKEY* load_pkey(const char* file_path, bool public_key) {
		if (!file_path) return nullptr;
		EVP_PKEY* key = nullptr;
		FILE* file = fopen(file_path, "r");
		if (file) {
			if (public_key)
				key = PEM_read_PUBKEY(file, nullptr, nullptr, nullptr);
			else
				key = PEM_read_PrivateKey(file, nullptr, nullptr, nullptr);
			fclose(file);
		}
		return key;
	}

	static void free_pkey(EVP_PKEY** key) {
		if (key) {
			if (*key) EVP_PKEY_free(*key);
			*key = nullptr;
		}
	}

	// Build EVP_PKEY from BIGNUMs via OSSL_PARAM + EVP_PKEY_fromdata.
	// p,g required; pub optional — when provided, builds a full peer key (SELECT_ALL),
	// otherwise only domain parameters (SELECT_DOMAIN_PARAMETERS).
	static EVP_PKEY* dh_build_from_bn(BIGNUM* p, BIGNUM* g, BIGNUM* pub = nullptr) {
		if (!p || !g) return nullptr;

		int p_len = BN_num_bytes(p);
		int g_len = BN_num_bytes(g);
		std::vector<unsigned char> p_bin(p_len);
		std::vector<unsigned char> g_bin(g_len);
		int p_len_final = BN_bn2nativepad(p, p_bin.data(), p_len);
		int g_len_final = BN_bn2nativepad(g, g_bin.data(), g_len);
		if (p_len_final <= 0 || g_len_final <= 0) return nullptr;

		OSSL_PARAM params[4];
		int idx = 0;
		params[idx++] = OSSL_PARAM_construct_BN("p", p_bin.data(), p_len_final);
		params[idx++] = OSSL_PARAM_construct_BN("g", g_bin.data(), g_len_final);

		std::vector<unsigned char> pub_bin;
		int pub_len_final = 0;
		if (pub) {
			int pub_len = BN_num_bytes(pub);
			pub_bin.resize(pub_len);
			pub_len_final = BN_bn2nativepad(pub, pub_bin.data(), pub_len);
			if (pub_len_final <= 0) return nullptr;
			params[idx++] = OSSL_PARAM_construct_BN("pub", pub_bin.data(), pub_len_final);
		}
		params[idx] = OSSL_PARAM_construct_end();

		int selection = pub ? OSSL_KEYMGMT_SELECT_ALL : OSSL_KEYMGMT_SELECT_DOMAIN_PARAMETERS;
		EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_DH, nullptr);
		if (!ctx) return nullptr;
		if (EVP_PKEY_fromdata_init(ctx) <= 0) {
			EVP_PKEY_CTX_free(ctx);
			return nullptr;
		}

		EVP_PKEY* dh = nullptr;
		if (EVP_PKEY_fromdata(ctx, &dh, selection, params) <= 0) {
			EVP_PKEY_CTX_free(ctx);
			return nullptr;
		}
		EVP_PKEY_CTX_free(ctx);
		return dh;
	}

	static EVP_PKEY* dh_create_params(BIGNUM* p, BIGNUM* g) {
		EVP_PKEY* dh = dh_build_from_bn(p, g);
		if (!dh) return nullptr;

		EVP_PKEY_CTX* ck_ctx = EVP_PKEY_CTX_new(dh, nullptr);
		int ck = ck_ctx ? EVP_PKEY_param_check(ck_ctx) : 0;
		EVP_PKEY_CTX_free(ck_ctx);
		if (ck != 1) {
			EVP_PKEY_free(dh);
			return nullptr;
		}
		return dh;
	}

	static EVP_PKEY* dh_keygen_from_params(EVP_PKEY* params) {
		if (!params) return nullptr;
		EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(params, nullptr);
		if (!ctx) return nullptr;
		if (EVP_PKEY_keygen_init(ctx) <= 0) {
			EVP_PKEY_CTX_free(ctx);
			return nullptr;
		}

		EVP_PKEY* key = nullptr;
		if (EVP_PKEY_keygen(ctx, &key) <= 0) {
			EVP_PKEY_CTX_free(ctx);
			return nullptr;
		}
		EVP_PKEY_CTX_free(ctx);

		EVP_PKEY_CTX* ck_ctx = EVP_PKEY_CTX_new(key, nullptr);
		if (!ck_ctx || EVP_PKEY_public_check(ck_ctx) != 1) {
			EVP_PKEY_CTX_free(ck_ctx);
			EVP_PKEY_free(key);
			return nullptr;
		}
		EVP_PKEY_CTX_free(ck_ctx);
		return key;
	}

	// 1 on success, 0 on fail and < 0 on error
	static int pkey_sign(unsigned char* dgst, int dlen, EVP_PKEY* pkey, unsigned char** sig, unsigned int* siglen) {
		OPSSL_CHECK_PARAM(dgst);
		OPSSL_CHECK_PARAM(pkey);
		OPSSL_CHECK_PARAM(sig);
		OPSSL_CHECK_PARAM(siglen);
		*sig = nullptr;
		*siglen = 0;

		EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pkey, nullptr);
		if (!ctx) return -100;

		int rc = -1;
		if (EVP_PKEY_sign_init(ctx) <= 0) goto END;
		rc = pkey_op(ctx, EVP_PKEY_sign, dgst, (unsigned int)dlen, sig, siglen);

	END:
		EVP_PKEY_CTX_free(ctx);
		if (rc <= 0) {
			free(*sig);
			*sig = nullptr;
			*siglen = 0;
			return rc;
		}
		return 1;
	}
	
	// 1 if the signature is valid, 0 if the signature is invalid and -1 on error
	static int pkey_verify(unsigned char* dgst, int dlen, EVP_PKEY* pkey, const unsigned char* sig, int siglen) {
		OPSSL_CHECK_PARAM(dgst);
		OPSSL_CHECK_PARAM(pkey);
		OPSSL_CHECK_PARAM(sig);
		EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pkey, nullptr);
		if (!ctx) return -1;
		if (EVP_PKEY_verify_init(ctx) <= 0) {
			EVP_PKEY_CTX_free(ctx);
			return -1;
		}
		int rc = EVP_PKEY_verify(ctx, sig, (size_t)siglen, dgst, dlen);
		EVP_PKEY_CTX_free(ctx);
		return rc;
	}

};

}

#endif
