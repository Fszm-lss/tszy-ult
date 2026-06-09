#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif
#include "mem_alloc.hpp"

namespace zbf {

struct tcp_message : object_tracker<tcp_message> {
    std::string data;
    std::string desc() const { return std::string("sz:") + std::to_string(data.size()); }
};

class tcp_message_protocol : object_tracker<tcp_message_protocol> {
public:
    virtual ~tcp_message_protocol() {}
    virtual tcp_message* genHeartbeat() = 0;
    virtual bool isHeartbeat(const tcp_message* msg) = 0;
    virtual uint32_t type(const tcp_message* msg) = 0;
    virtual uint32_t headerSize() const = 0;
    virtual uint32_t bodySize(const tcp_message* msg) = 0;
    virtual uint32_t MaxBodySize() { return 1048576*16; /*16M*/ }
    enum { Type_NaN = 0x00, Heartbeat = 0x01 };
};

// header[4] | body[size]
class default_msg_proto : public tcp_message_protocol {
public:
    virtual tcp_message* genHeartbeat() override {
        return genMessage(Heartbeat, "");
    }
    virtual bool isHeartbeat(const tcp_message* msg) override {
        return type(msg) == Heartbeat;
    }
    virtual uint32_t type(const tcp_message* msg) override {
        if (msg == nullptr || msg->data.size() < sizeof(uint32_t)) return Type_NaN;
        uint32_t hdr = extract_uint32(msg->data);
        return (hdr & 0xFF000000) >> 24; // high 8 bits
    }
    virtual uint32_t headerSize() const override {
        return sizeof(uint32_t);
    }
    virtual uint32_t bodySize(const tcp_message* msg) override {
        if (msg == nullptr || msg->data.size() < sizeof(uint32_t)) return 0;
        uint32_t hdr = extract_uint32(msg->data);
        return hdr & 0xFFFFFF; // low 24 bits
    }

public:
    tcp_message* genMessage(uint8_t type, const std::string& body) {
        if (body.size() > MaxBodySize()) return nullptr;
        tcp_message* msg = new tcp_message;
        uint32_t hdr = type;
        hdr = (hdr << 24) | body.size();
        append_uint32(msg->data, hdr);
        msg->data += body;
        return msg;
    }

    static uint32_t extract_uint32(const std::string& data) {
        if (data.size() < sizeof(uint32_t)) return 0;
        uint32_t val = 0;
        memcpy(&val, data.data(), sizeof(val));
        val = ::ntohl(val);
        return val;
    }

    static void append_uint32(std::string& data, uint32_t val) {
        std::string str;
        str.resize(sizeof(uint32_t));
        val = ::htonl(val);
        memcpy(str.data(), &val, sizeof(val));
        data += str;
    }
};

}
