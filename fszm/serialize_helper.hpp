#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace fszm {

// string contains raw data, maybe not terminated with '\0'
typedef std::vector<std::string>   DataRow;
typedef std::vector<DataRow>       DataTable;
typedef std::vector<DataTable>     DataTables;
typedef std::vector<unsigned int>  FieldsType;
typedef std::map<std::string, std::string>           StringMap;
typedef std::vector<std::pair<double, std::string> > ScoreArray;

class serialize_helper {
public:
    serialize_helper() {
    }

    serialize_helper(const std::string& buffer) : _stringbuf(buffer) {
    }

    std::string data() {
        return _oss.str();
    }

    void reset() {
        reset(std::string());
    }

    void reset(const std::string& buffer) {
        _stringbuf = buffer;
        _oss.str("");
        _oss.clear();
    }

    void serialize_int8(int8_t val)   { serialize_uint8((uint8_t) val);        }
    int8_t deserialize_int8()         { return (int8_t) deserialize_uint8();   }
    void serialize_int16(int16_t val) { serialize_uint16((uint16_t) val);      }
    int16_t deserialize_int16()       { return (int16_t) deserialize_uint16(); }
    void serialize_int32(int32_t val) { serialize_uint32((uint32_t) val);      }
    int32_t deserialize_int32()       { return (int32_t) deserialize_uint32(); }
    void serialize_int64(int64_t val) { serialize_uint64((uint64_t) val);      }
    int64_t deserialize_int64()       { return (int64_t) deserialize_uint64(); }

    void serialize_uint8(uint8_t val) {
        _oss.write(reinterpret_cast<const char*>(&val), sizeof(val));
    }
    uint8_t deserialize_uint8() {
        if (_stringbuf.size() < sizeof(uint8_t)) return 0;
        uint8_t val = 0;
        std::string val_str = _stringbuf.substr(0, sizeof(val));
        val = static_cast<uint8_t>(val_str[0]);
        _stringbuf = _stringbuf.substr(sizeof(val));
        return val;
    }

    void serialize_uint16(uint16_t local) {
        uint16_t val = htons(local);
        _oss.write(reinterpret_cast<const char*>(&val), sizeof(val));
    }
    uint16_t deserialize_uint16() {
        if (_stringbuf.size() < sizeof(uint16_t)) return 0;
        uint16_t val = 0;
        std::string val_str = _stringbuf.substr(0, sizeof(val));
        memcpy(&val, val_str.data(), sizeof(val));
        val = ntohs(val);
        _stringbuf = _stringbuf.substr(sizeof(val));
        return val;
    }

    void serialize_uint32(uint32_t local) {
        uint32_t val = htonl(local);
        _oss.write(reinterpret_cast<const char*>(&val), sizeof(val));
    }
    uint32_t deserialize_uint32() {
        if (_stringbuf.size() < sizeof(uint32_t)) return 0;
        uint32_t val = 0;
        std::string val_str = _stringbuf.substr(0, sizeof(val));
        memcpy(&val, val_str.data(), sizeof(val));
        val = ntohl(val);
        _stringbuf = _stringbuf.substr(sizeof(val));
        return val;
    }

    void serialize_uint64(uint64_t val) {
        uint32_t high = static_cast<uint32_t>(val >> 32);
        uint32_t low  = static_cast<uint32_t>(val);
        serialize_uint32(high);
        serialize_uint32(low);
    }
    uint64_t deserialize_uint64() {
        if (_stringbuf.size() < sizeof(uint64_t)) return 0;
        uint32_t high = deserialize_uint32();
        uint32_t low  = deserialize_uint32();
        uint64_t val = (static_cast<uint64_t>(high) << 32) | low;
        return val;
    }

    void serialize_bool(bool val) {
        serialize_uint8(val ? 1 : 0);
    }
    bool deserialize_bool() {
        uint8_t val = deserialize_uint8();
        return val == 1;
    }

    void serialize_double(double val) {
        uint64_t bits = 0;
        std::memcpy(&bits, &val, sizeof(bits));
        serialize_uint64(bits);
    }
    double deserialize_double() {
        uint64_t bits = deserialize_uint64();
        double val = 0.0;
        std::memcpy(&val, &bits, sizeof(val));
        return val;
    }

    void serialize_float(float val) {
        uint32_t bits = 0;
        std::memcpy(&bits, &val, sizeof(bits));
        serialize_uint32(bits);
    }
    float deserialize_float() {
        uint32_t bits = deserialize_uint32();
        float val = 0.0f;
        std::memcpy(&val, &bits, sizeof(val));
        return val;
    }

    void serialize_str(const std::string& data) {
        serialize_uint32(data.size());
        _oss.write(data.data(), data.size());
    }
    std::string deserialize_str() {
        uint32_t size = deserialize_uint32();
        if (_stringbuf.size() < size) return std::string();
        std::string data = _stringbuf.substr(0, size);
        _stringbuf = _stringbuf.substr(size);
        return data;
    }

    void serialize_row(const DataRow& row) {
        serialize_uint32(row.size());
        for (const std::string& str : row) {
            serialize_str(str);
        }
    }
    DataRow deserialize_row() {
        DataRow row;
        uint32_t size = deserialize_uint32();
        for (uint32_t i = 0; i < size; ++i) {
            std::string str = deserialize_str();
            row.push_back(str);
        }
        return row;
    }

    void serialize_table(const DataTable& table) {
        serialize_uint32(table.size());
        for (const DataRow& row : table) {
            serialize_row(row);
        }
    }
    DataTable deserialize_table() {
        DataTable table;
        uint32_t size = deserialize_uint32();
        for (uint32_t i = 0; i < size; ++i) {
            DataRow row = deserialize_row();
            table.push_back(row);
        }
        return table;
    }

    void serialize_tables(const DataTables& tables) {
        serialize_uint32(tables.size());
        for (const DataTable& tab : tables) {
            serialize_table(tab);
        }
    }
    DataTables deserialize_tables() {
        DataTables tables;
        uint32_t size = deserialize_uint32();
        for (uint32_t i = 0; i < size; ++i) {
            DataTable tab = deserialize_table();
            tables.push_back(tab);
        }
        return tables;
    }

    void serialize_fields_type(const FieldsType& fieldsType) {
        serialize_uint32(fieldsType.size());
        for (const unsigned int& ft : fieldsType) {
            serialize_uint32(ft);
        }
    }
    FieldsType deserialize_fields_type() {
        FieldsType fieldsType;
        uint32_t size = deserialize_uint32();
        for (uint32_t i = 0; i < size; ++i) {
            unsigned int ft = deserialize_uint32();
            fieldsType.push_back(ft);
        }
        return fieldsType;
    }

    void serialize_string_map(const StringMap& map) {
        serialize_uint32(map.size());
        for (const auto& kv : map) {
            serialize_str(kv.first);
            serialize_str(kv.second);
        }
    }
    StringMap deserialize_string_map() {
        StringMap map;
        uint32_t size = deserialize_uint32();
        for (uint32_t i = 0; i < size; ++i) {
            std::string key = deserialize_str();
            std::string val = deserialize_str();
            map[key] = val;
        }
        return map;
    }

    void serialize_score_array(const ScoreArray& arr) {
        serialize_uint32(arr.size());
        for (const auto& item : arr) {
            serialize_double(item.first);
            serialize_str(item.second);
        }
    }
    ScoreArray deserialize_score_array() {
        ScoreArray arr;
        uint32_t size = deserialize_uint32();
        for (uint32_t i = 0; i < size; ++i) {
            double score = deserialize_double();
            std::string val = deserialize_str();
            arr.push_back(std::make_pair(score, val));
        }
        return arr;
    }

protected:
    std::string _stringbuf;
    std::ostringstream _oss;
};

}
