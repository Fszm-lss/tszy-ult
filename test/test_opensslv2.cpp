
#include <cassert>
#include <unordered_map>
#include "fszm/random_utils.hpp"
#include "fszm/openssl_utils_v2.hpp"
#include <openssl/ec.h>


using namespace fszm;

static void test_ecc(int curve_nid);

static void test_dh(int bits);

static void test_rsa();
static void test_dsa();
static void test_symmetric(int len);

int main(int argc, char** argv)
{
    std::string line(100, '-');

    std::string strIn = random_utils::randomString(fszm::NumbersAndLetters, 50);
    std::string md5 = openssl_utils::md5(strIn);
    std::string sha1 = openssl_utils::sha1(strIn);
    printf("strIn =%s\n", strIn.c_str());
    printf("md5   =%s\n", md5.c_str());
    printf("sha1  =%s\n", sha1.c_str());

	std::string strb64 = openssl_utils::base64_encode(strIn);
	printf("base64=%s\n", strb64.c_str());
	strIn = openssl_utils::base64_decode(strb64);
    printf("decode=%s\n", strIn.c_str());

	int prime_len = 1024;
    test_dh(prime_len);
    printf("%s\n", line.c_str());

    test_ecc(NID_X9_62_prime256v1);
    printf("%s\n", line.c_str());

	test_rsa();
	printf("%s\n", line.c_str());

	test_dsa();
	printf("%s\n", line.c_str());
	
	test_symmetric(4321);

    return 0;
}

void test_rsa() {
	openssl_utils::eEncryptType enc = openssl_utils::eEncryptType::eET_RSA_PUBLIC_ENC;
	openssl_utils::eEncryptType dec = openssl_utils::eEncryptType::eET_RSA_PRIVATE_DEC;

	int rc = 0;
	std::string data = random_utils::randomString(fszm::NumbersAndLetters, 164);
	unsigned char* dst = nullptr;
	unsigned int dstlen = 0;
	unsigned char* src = nullptr;
	unsigned int srclen = 0;

	EVP_PKEY* pubKey = openssl_utils::load_rsa_key("key/rsa_public_key.pem", true);
	assert(pubKey != nullptr);
	EVP_PKEY* priKey = openssl_utils::load_rsa_key("key/rsa_private_key.pem", false);
	assert(priKey != nullptr);

	printf("data\t%s\n", data.c_str());
	rc = openssl_utils::rsa_encrypt((unsigned char*)data.c_str(), data.length(), pubKey, enc, &dst, &dstlen);
	openssl_utils::free_rsa_key(&pubKey);	
	rc = openssl_utils::rsa_encrypt(dst, dstlen, priKey, dec, &src, &srclen);
	openssl_utils::free_rsa_key(&priKey);
	std::string str((char*)src, srclen);
	printf(" src\t%s\n", str.c_str());

	openssl_utils::free_rsa_key(&pubKey);
	openssl_utils::free_rsa_key(&priKey);
}

void test_dsa() {
	EVP_PKEY* pubKey = openssl_utils::load_rsa_key("key/dsa_public_key.pem", true);
	assert(pubKey != nullptr);
	EVP_PKEY* priKey = openssl_utils::load_rsa_key("key/dsa_private_key.pem", false);
	assert(priKey != nullptr);

	std::string str = random_utils::randomString(fszm::NumbersAndLetters, 64);
	unsigned char* signature = 0;
	unsigned int siglen = 0;
	int rc1 = openssl_utils::dsa_sign((unsigned char*)str.c_str(), str.size(), priKey, &signature, &siglen);
	int rc2 = openssl_utils::ecc_verify((unsigned char*)str.c_str(), str.size(), pubKey, signature, siglen);
	printf("STR: %s\n", str.c_str());
    printf("Sign by key1(dsa/priv): %s\n", openssl_utils::hex2string(signature, siglen).c_str());
    printf("Verify by key1(dsa/pub): %s\n", rc2 == 1 ? "PASS" : "not PASS");
    free(signature);

	openssl_utils::free_dsa_key(&pubKey);
	openssl_utils::free_dsa_key(&priKey);
}

void test_symmetric(int len) {
	std::unordered_map<int, std::string> encTypeDesc;
    encTypeDesc[openssl_utils::eET_DES_ECB]      = "DES-ECB[key: 64bit]";
    encTypeDesc[openssl_utils::eET_DES_CBC]      = "DES-CBC[key/ivec: 64bit/64bit]";
    encTypeDesc[openssl_utils::eET_DES_CFB]      = "DES-CFB[key/ivec: 64bit/64bit]";
    encTypeDesc[openssl_utils::eET_DES_OFB]      = "DES-OFB[key/ivec: 64bit/64bit]";
    encTypeDesc[openssl_utils::eET_3DES_ECB]     = "3DES-ECB[key: 192bit]";
    encTypeDesc[openssl_utils::eET_3DES_CBC]     = "3DES-CBC[key/ivec: 192bit/64bit]";
    encTypeDesc[openssl_utils::eET_3DES_CFB]     = "3DES-CFB[key/ivec: 192bit/64bit]";
    encTypeDesc[openssl_utils::eET_3DES_OFB]     = "3DES-OFB[key/ivec: 192bit/64bit]";
    encTypeDesc[openssl_utils::eET_CAMELLIA_ECB] = "CAMELLIA-ECB[key: 128bit]";
    encTypeDesc[openssl_utils::eET_CAMELLIA_CBC] = "CAMELLIA-CBC[key/ivec: 128bit/128bit]";
    encTypeDesc[openssl_utils::eET_CAMELLIA_CFB] = "CAMELLIA-CFB[key/ivec: 128bit/128bit]";
    encTypeDesc[openssl_utils::eET_CAMELLIA_OFB] = "CAMELLIA-OFB[key/ivec: 128bit/128bit]";
    encTypeDesc[openssl_utils::eET_CAMELLIA_CTR] = "CAMELLIA-CTR[key/ivec: 128bit/128bit]";
    encTypeDesc[openssl_utils::eET_AES_ECB]      = "AES-ECB[key: 128bit]";
    encTypeDesc[openssl_utils::eET_AES_CBC]      = "AES-CBC[key/ivec: 128bit/128bit]";
    encTypeDesc[openssl_utils::eET_AES_CFB]      = "AES-CFB[key/ivec: 128bit/128bit]";
    encTypeDesc[openssl_utils::eET_AES_OFB]      = "AES-OFB[key/ivec: 128bit/128bit]";
    encTypeDesc[openssl_utils::eET_AES_CTR]      = "AES-CTR[key/ivec: 128bit/128bit]";
    
    int rc = 0;
    std::string in_str  = random_utils::randomString(fszm::NumbersAndLetters, len);
    std::string in_key  = random_utils::randomString(fszm::NumbersAndLetters, 24);
    std::string in_ivec = random_utils::randomString(fszm::NumbersAndLetters, 16);
    
    for (int i = openssl_utils::eET_DES_ECB; i <= openssl_utils::eET_AES_CTR; ++i) {
        openssl_utils::eEncryptType type = (openssl_utils::eEncryptType) i;
        std::string enc_str;
        std::string dec_str;
        int dec_len = 0;

        rc = openssl_utils::symmetric_encrypt(in_str, in_key, in_ivec, type, true, enc_str);
        if (rc) {
            printf("symmetric encrypt fail, rc=%d, srclen=%ld, type=%s\n", rc, in_str.size(), encTypeDesc[type].c_str());
            continue;
        }

        rc = openssl_utils::symmetric_encrypt(enc_str, in_key, in_ivec, type, false, dec_str);
        if (rc) {
            printf("symmetric decrypt fail, rc=%d, enclen=%ld, type=%s\n", rc, enc_str.size(), encTypeDesc[type].c_str());
            continue;
        }

        dec_len = dec_str.size();
        if (dec_len > in_str.size()) {
            dec_str = dec_str.substr(0, in_str.size());
        }
        if (in_str.compare(dec_str) != 0) {
            printf("symmetric decrypt fail(dec != src), declen=%ld, type=%s\n", dec_str.size(), encTypeDesc[type].c_str());
            continue;
        }

        printf("symmetric encrypt/decrypt success, src/dec=%ld/%d, type=%s\n", in_str.size(), dec_len, encTypeDesc[type].c_str());
    }
}

void test_ecc(int curve_nid)
{
	printf("curve name(sn): %s\n", OBJ_nid2sn(curve_nid));
	printf("curve name(nist): %s\n", EC_curve_nid2nist(curve_nid));

	// 1. create key pair by same curve_nid
	EVP_PKEY* key1 = nullptr;
	openssl_utils::ecc_create(curve_nid, &key1);
	EVP_PKEY* key2 = nullptr;
	openssl_utils::ecc_create(curve_nid, &key2);

	// 2. share pub key with each other (raw bytes, ready for network transmission)
	std::string pubkey1 = openssl_utils::ecc_export_pubkey(key1);
	std::string pubkey2 = openssl_utils::ecc_export_pubkey(key2);
	printf("PUBKEY1(archive)  : %s\n", openssl_utils::hex2string((unsigned char*)pubkey1.data(), pubkey1.size()).c_str());
	printf("PUBKEY2(archive)  : %s\n", openssl_utils::hex2string((unsigned char*)pubkey2.data(), pubkey2.size()).c_str());

	// 3. make share key
	unsigned char sharekey1[256] = {0};
	unsigned char sharekey2[256] = {0};
	int len1 = openssl_utils::ecc_compute_sharekey(key1, curve_nid, pubkey2, sharekey1, sizeof(sharekey1));
	int len2 = openssl_utils::ecc_compute_sharekey(key2, curve_nid, pubkey1, sharekey2, sizeof(sharekey2));
    // compare shared key
    printf("sharekey1: %s\n", openssl_utils::hex2string(sharekey1, len1).c_str());
    printf("sharekey2: %s\n", openssl_utils::hex2string(sharekey2, len2).c_str());
    bool equal = (memcmp(sharekey1, sharekey2, len1) == 0);
	printf("sharekey1 and sharekey2 is %s\n", equal ? "EQUAL" : "not EQUAL");

	// sign / verify (sign with private key, verify with imported public key)
	std::string str = random_utils::randomString(fszm::NumbersAndLetters, 32);
	unsigned char* signature = 0;
	unsigned int siglen = 0;
	EVP_PKEY* pubkey1_only = openssl_utils::ecc_import_pubkey(curve_nid, pubkey1);
	int rc1 = openssl_utils::ecc_sign((unsigned char*)str.c_str(), str.size(), key1, &signature, &siglen);
	int rc2 = openssl_utils::ecc_verify((unsigned char*)str.c_str(), str.size(), pubkey1_only, signature, siglen);
	printf("STR: %s\n", str.c_str());
    printf("Sign by key1(ecc/priv): %s\n", openssl_utils::hex2string(signature, siglen).c_str());
    printf("Verify by key1(ecc/pub): %s\n", rc2 == 1 ? "PASS" : "not PASS");
    free(signature);

	openssl_utils::ecc_destroy(&pubkey1_only);
	openssl_utils::ecc_destroy(&key1);
	openssl_utils::ecc_destroy(&key2);
}

// Key exchange process
// 1. Client encrypts exchange info and temporary symmetric key using RSA public key, sends to server; server decrypts using RSA private key
// 2. Server encrypts exchange info using temporary key, sends to client; client decrypts using temporary symmetric key
// 3. Client / server each generate a session symmetric key from the paired exchange info
// 4. Client and server communicate using the session symmetric key
void test_dh(int bits)
{
	printf("DH bits: %d\n", bits);

	// 1. initiator generates keypair
	EVP_PKEY* dh1 = openssl_utils::dh_generate(bits);

	// 2. share dh1's P & G (raw bytes, ready for network transmission)
	std::string p = openssl_utils::dh_export(dh1, "p");
	std::string g = openssl_utils::dh_export(dh1, "g");
	printf("P(archive)  : %s\n", openssl_utils::hex2string((unsigned char*)p.data(), p.size()).c_str());
	printf("G(archive)  : %s\n", openssl_utils::hex2string((unsigned char*)g.data(), g.size()).c_str());

	// 3. responder generates keypair from P & G
	EVP_PKEY* dh2 = openssl_utils::dh_generate(p, g);

	// 4. share pub keys with each other
	std::string pubkey1 = openssl_utils::dh_export(dh1, "pub");
	std::string pubkey2 = openssl_utils::dh_export(dh2, "pub");
	printf("PUBKEY1(archive)  : %s\n", openssl_utils::hex2string((unsigned char*)pubkey1.data(), pubkey1.size()).c_str());
	printf("PUBKEY2(archive)  : %s\n", openssl_utils::hex2string((unsigned char*)pubkey2.data(), pubkey2.size()).c_str());

	// 5. compute shared secret
	unsigned char key1[256] = {0};
	unsigned char key2[256] = {0};
	int len1 = openssl_utils::dh_compute_sharekey(dh1, pubkey2, key1, sizeof(key1));
	int len2 = openssl_utils::dh_compute_sharekey(dh2, pubkey1, key2, sizeof(key2));

	printf("sharekey1: %s\n", openssl_utils::hex2string(key1, len1).c_str());
	printf("sharekey2: %s\n", openssl_utils::hex2string(key2, len2).c_str());
	bool equal = (memcmp(key1, key2, len1) == 0);
	printf("sharekey1 and sharekey2 is %s\n", equal ? "EQUAL" : "not EQUAL");

	openssl_utils::dh_destroy(&dh1);
	openssl_utils::dh_destroy(&dh2);
}
