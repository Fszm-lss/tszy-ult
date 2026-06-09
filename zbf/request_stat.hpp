#pragma once

#include <algorithm>
#include <climits>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "timer_helper.hpp"
#include "socket_utils.hpp"

namespace zbf {

struct RequestBill {
    uint32_t requestType;
    long     startTime;
    int      costTime;
};

struct StatBill {
    uint32_t requestType;
    int      maxCost;
    int      minCost;
    int      avgCost;
    int      sampleSize;
};

typedef std::unordered_map<uint32_t, std::vector<RequestBill> > RequestBillMap;

// thread unsafe
class RequestStat {
public:
    void onReqStart(const uint32_t requestType) {
        _reqStartTime[requestType] = socket_utils::currentTimeMillis();
    }

    void onReqFinish(const uint32_t requestType) {
        auto it = _reqStartTime.find(requestType);
        if (it == _reqStartTime.end()) return;
        long start = it->second;
        long now   = socket_utils::currentTimeMillis();
        if (start != socket_utils::TimeStampErr && now != socket_utils::TimeStampErr) {
            RequestBill bill;
            bill.requestType = requestType;
            bill.startTime   = start;
            bill.costTime    = now - start;
            std::lock_guard<std::mutex> lock(_lockReqBill);
            _requestBill[requestType].push_back(bill);
        }
    }

    RequestBillMap consume() {
        std::lock_guard<std::mutex> lock(_lockReqBill);
        RequestBillMap billMap = _requestBill;
        _requestBill.clear();
        return billMap;
    }

private:
    RequestBillMap                     _requestBill;
    std::mutex                         _lockReqBill;
    std::unordered_map<uint32_t, long> _reqStartTime;
};

class ReqStatLogger {
public:
    // default 5 minutes
    ReqStatLogger(int interval = 5, zbf::TickUnit unit = zbf::Minute)
    : _interval(interval), _tmHelper(unit) {
    }

    void start(short thdId) {
        _tmHelper.addTimerTask(_interval, onStat, this);
        _tmHelper.start(thdId);
    }

    void stop() {
        _tmHelper.stop();
        _allStat.clear();
    }

    void add(RequestStat* reqStat) {
        std::lock_guard<std::mutex> lock(_lockAllStat);
        _allStat.push_back(reqStat);
    }

private:
    static bool onStat(void* param) {
        ReqStatLogger* self = (ReqStatLogger*) param;        
        std::vector<RequestStat*> allStat;
        {
            std::lock_guard<std::mutex> lock(self->_lockAllStat);
            allStat = self->_allStat;
        }
        
        for (RequestStat* reqStat : allStat) {
            RequestBillMap billMap = reqStat->consume();
            if (billMap.empty()) continue;

            for (auto& pair : billMap) {
                if (pair.second.empty()) continue;
                StatBill statBill;
                statBill.requestType = pair.first;
                std::vector<RequestBill>& reqBills = pair.second;
                statBill.sampleSize = reqBills.size();
                statBill.maxCost = 0;
                statBill.minCost = INT32_MAX;
                int totalCost = 0;
                for (RequestBill& reqBill : reqBills) {
                    self->logRequestBill(reqBill);
                    statBill.maxCost = std::max(statBill.maxCost, reqBill.costTime);
                    statBill.minCost = std::min(statBill.minCost, reqBill.costTime);
                    totalCost += reqBill.costTime;
                }
                statBill.avgCost = totalCost / statBill.sampleSize;
                self->logStatBill(statBill);
            }
        }
        return true;
    }

    void logStatBill(const StatBill& statBill) {
        LOG_MSG(LogLevel::Info, "type=0x%x, sams=%d, avg/max=%d/%d",
            statBill.requestType, statBill.sampleSize, statBill.avgCost, statBill.maxCost);
    }

    void logRequestBill(const RequestBill& reqBill) {
    }

private:
    int          _interval;
    timer_helper _tmHelper;
    std::vector<RequestStat*> _allStat;
    std::mutex                _lockAllStat;
};

}
