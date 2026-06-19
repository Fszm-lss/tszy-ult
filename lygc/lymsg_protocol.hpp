#ifndef lymsg_protocol_hpp
#define lymsg_protocol_hpp
// 2025-9

#include <cinttypes>
#include <cstdio>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif
#include "zbf/tcp_message.hpp"
#include "zbf/socket_tcp_v5.hpp"

// ---- byte order conversion switch ----
// Define LYMSG_NETWORK_BYTE_ORDER to convert header fields between host and network byte order.
// When undefined (default), fields use host byte order, suitable for homogeneous x86_64 deployments.
// #define LYMSG_NETWORK_BYTE_ORDER

namespace lygc {
using zbf::tcp_message_protocol;
using zbf::tcp_message;

#pragma pack(1)
struct lymsg_header {
    uint16_t origin;  // 2    
    uint16_t reserve; // 2
    uint32_t type;    // 4
    uint64_t serial;  // 8
    uint32_t size;    // 4, body size    
};
#pragma pack()


#ifdef LYMSG_NETWORK_BYTE_ORDER

// 64-bit network/host byte order conversion (no standard htonll/ntohll in POSIX)
inline uint64_t htonll(uint64_t host) {
    uint32_t lo = (uint32_t)(host & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)(host >> 32);
    return ((uint64_t)htonl(lo) << 32) | (uint64_t)htonl(hi);
}
inline uint64_t ntohll(uint64_t net) {
    uint32_t lo = (uint32_t)(net & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)(net >> 32);
    return ((uint64_t)ntohl(hi) << 32) | (uint64_t)ntohl(lo);
}
inline void hton(lymsg_header& header) {
    header.origin = htons(header.origin);
    header.reserve = htons(header.reserve);
    header.type   = htonl(header.type);
    header.serial = htonll(header.serial);
    header.size   = htonl(header.size);    
}
inline void ntoh(lymsg_header& header) {
    header.origin = ntohs(header.origin);
    header.reserve = ntohs(header.reserve);
    header.type   = ntohl(header.type);
    header.serial = ntohll(header.serial);
    header.size   = ntohl(header.size);    
}

#else

inline uint64_t htonll(uint64_t host) { return host; }
inline uint64_t ntohll(uint64_t net)  { return net; }
inline void hton(lymsg_header&) {}
inline void ntoh(lymsg_header&) {}

#endif // LYMSG_NETWORK_BYTE_ORDER

#define LYMSG_TYPE_REQ           0x00000000 // request  flag
#define LYMSG_TYPE_RESP          0x80000000 // response flag
#define LYMSG_TYPE_ENC           0x40000000 // encrypt  flag
#define LYMSG_TYPE_HANDSHAKE     0x00000011 // handshake msg
#define LYMSG_TYPE_DB_MYSQL      0x00000021 
#define LYMSG_TYPE_DB_MONGO      0x00000022
#define LYMSG_TYPE_RESERVE       0x000000FF

#define LYMSG_DESC(pHeader)  lygc::lymsg_helper::lymsg_desc(pHeader)

class lymsg_protocol : public tcp_message_protocol {
public:
    lymsg_protocol() {}

    virtual ~lymsg_protocol() {}

    virtual tcp_message* genHeartbeat() override {
        lymsg_header header;
        memset(&header, 0, sizeof(lymsg_header));
        header.type = Heartbeat;
        header.size = 0;
        hton(header);
        tcp_message* msg = new tcp_message();
        msg->data.assign((const char*)&header, sizeof(lymsg_header));
        return msg;
    }

    virtual bool isHeartbeat(const tcp_message* msg) override {
        if (!msg) return false;
        return type(msg) == Heartbeat;
    }

    virtual uint32_t type(const tcp_message* msg) override {
        if (!msg || msg->data.size() < sizeof(lymsg_header)) return Type_NaN;
        lymsg_header header;
        memcpy(&header, msg->data.data(), sizeof(lymsg_header));
#ifdef LYMSG_NETWORK_BYTE_ORDER
        return ::ntohl(header.type);
#else
        return header.type;
#endif
    }

    virtual uint32_t headerSize() const override { return sizeof(lymsg_header); }

    virtual uint32_t bodySize(const tcp_message* msg) override {
        if (!msg || msg->data.size() < sizeof(lymsg_header)) return 0;
        lymsg_header header;
        memcpy(&header, msg->data.data(), sizeof(lymsg_header));
#ifdef LYMSG_NETWORK_BYTE_ORDER
        return ::ntohl(header.size);
#else
        return header.size;
#endif
    }
};


class lymsg_helper {
public:
    // notice data is binary
    static tcp_message* packMsg(const lymsg_header* header, const std::string& data) {
        tcp_message* msg = new tcp_message();
        msg->data.resize(sizeof(lymsg_header) + data.size());

        lymsg_header* pHdr = reinterpret_cast<lymsg_header*>(msg->data.data());
        memcpy(pHdr, header, sizeof(lymsg_header));
        pHdr->size = data.size();
        hton(*pHdr);

        if (data.size() > 0)
            memcpy(msg->data.data() + sizeof(lymsg_header), data.data(), data.size());
        return msg;
    }

    // notice data can be raw-binary/PB/json depends on header's type
    static int unpackMsg(const tcp_message* msg, lymsg_header& header, std::string& data) {
        if (!msg || msg->data.size() < sizeof(lymsg_header)) {
            return -1;
        }
        memcpy(&header, msg->data.data(), sizeof(lymsg_header));
        ntoh(header);
        data = msg->data.substr(sizeof(lymsg_header));
        return 0;
    }

    static lymsg_header* copyHeader(const lymsg_header* header) {
        lymsg_header* copy = new lymsg_header;
        memcpy(copy, header, sizeof(lymsg_header));
        return copy;
    }

    static std::string lymsg_desc(const lymsg_header* header) {
        char szDesc[64] = { 0 };
        const char* fmt = "[%u|0x%x|0x%x|%" PRIu64 "|%u]";
        snprintf(szDesc, sizeof(szDesc), fmt, header->origin, header->reserve, header->type, header->serial, header->size);
        return std::string(szDesc);
    }
};

inline std::string strBrief(const std::string& src, int maxVisiable = 50) {
    if (src.length() <= maxVisiable) return src;
    return src.substr(0, maxVisiable) + "..." + std::to_string(src.size());
}


}

#endif
