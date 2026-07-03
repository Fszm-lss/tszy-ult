#pragma once

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "timer_helper.hpp"
#include "socket_utils.hpp"

namespace zbf {

struct Histogram {
    static constexpr int kNumBuckets  = 14;
    static constexpr int kLimits[kNumBuckets - 1] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000};

    void observe(int costMs) {
        if (costMs < 0) return;

        int idx = 0;
        while (idx < kNumBuckets - 1 && costMs > kLimits[idx]) ++idx;

        buckets[idx].fetch_add(1, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_relaxed);
        sum.fetch_add(costMs, std::memory_order_relaxed);

        int cur = maxCost.load(std::memory_order_relaxed);
        while (costMs > cur && !maxCost.compare_exchange_weak(cur, costMs, std::memory_order_relaxed)) {}

        cur = minCost.load(std::memory_order_relaxed);
        while (costMs < cur && !minCost.compare_exchange_weak(cur, costMs, std::memory_order_relaxed)) {}
    }

    struct Snapshot {
        int     buckets[kNumBuckets];
        int     count;
        int64_t sum;
        int     maxCost;
        int     minCost;
    };

    Snapshot snapshot() {
        Snapshot snap;
        for (int i = 0; i < kNumBuckets; ++i) {
            snap.buckets[i] = buckets[i].exchange(0, std::memory_order_relaxed);
        }
        snap.count   = count.exchange(0, std::memory_order_relaxed);
        snap.sum     = sum.exchange(0, std::memory_order_relaxed);
        snap.maxCost = maxCost.exchange(0, std::memory_order_relaxed);
        snap.minCost = minCost.exchange(INT_MAX, std::memory_order_relaxed);
        return snap;
    }

    static int percentile(double p, int total, const int buckets[kNumBuckets], int maxCost) {
        if (total == 0) return 0;
        int target = static_cast<int>(total * p);
        if (target >= total) target = total - 1;

        int acc = 0;
        for (int i = 0; i < kNumBuckets; ++i) {
            acc += buckets[i];
            if (acc > target) {
                int lower   = (i == 0) ? 0 : kLimits[i - 1];
                int upper   = (i == kNumBuckets - 1) ? maxCost : std::min(maxCost, kLimits[i]);
                int inBucket = buckets[i];
                if (inBucket == 0) return lower;
                int prevAcc = acc - inBucket;
                int needed  = target - prevAcc;
                return lower + (needed * (upper - lower)) / inBucket;
            }
        }
        return maxCost;
    }

    std::atomic<int>     buckets[kNumBuckets] {};
    std::atomic<int>     count  {0};
    std::atomic<int64_t> sum    {0};
    std::atomic<int>     maxCost{0};
    std::atomic<int>     minCost{INT_MAX};
};

struct StatBill {
    int      userThdId     = 0;
    uint32_t requestType   = 0;
    int      totalCount    = 0;
    int64_t  totalReqBytes = 0;
    int64_t  totalRspBytes = 0;
    uint32_t inflightCount  = 0;
    uint32_t maxQueueDepth  = 0;

    int avgCost = 0;
    int maxCost = 0;
    int minCost = 0;
    int p50 = 0, p80 = 0, p90 = 0, p95 = 0, p99 = 0;
};

// per thread
class RequestStat {
public:
    explicit RequestStat(int userThdId) : _userThdId(userThdId) {}

    void onReqStart(uint32_t requestType) {
        _inflightCount.fetch_add(1, std::memory_order_relaxed);
        _reqStartTime[requestType] = socket_utils::currentTimeMillis();
    }

    void onReqFinish(uint32_t requestType, uint32_t reqBytes = 0, uint32_t rspBytes = 0) {
        auto it = _reqStartTime.find(requestType);
        if (it == _reqStartTime.end()) return;

        _inflightCount.fetch_sub(1, std::memory_order_relaxed);

        long start = it->second;
        _reqStartTime.erase(it);

        long now = socket_utils::currentTimeMillis();
        if (start == socket_utils::TimeStampErr || now == socket_utils::TimeStampErr) return;
        int costTime = static_cast<int>(now - start);

        PerTypeMetrics& m = getOrCreateMetrics(requestType);
        m.totalCount.fetch_add(1, std::memory_order_relaxed);
        m.totalReqBytes.fetch_add(reqBytes, std::memory_order_relaxed);
        m.totalRspBytes.fetch_add(rspBytes, std::memory_order_relaxed);
        m.latencyHist.observe(costTime);
    }

    void sampleQueueDepth(uint32_t depth) {
        uint32_t cur = _maxQueueDepth.load(std::memory_order_relaxed);
        while (depth > cur && !_maxQueueDepth.compare_exchange_weak(cur, depth, std::memory_order_relaxed)) {}
    }

    uint32_t inflightCount() const {
        return _inflightCount.load(std::memory_order_relaxed);
    }

    struct TypeSnapshot {
        uint32_t requestType;
        int      totalCount;
        int64_t  totalReqBytes;
        int64_t  totalRspBytes;
        Histogram::Snapshot histSnap;
    };

    struct Snapshot {
        int                        userThdId;
        uint32_t                   inflightCount;
        uint32_t                   maxQueueDepth;
        std::vector<TypeSnapshot>  types;
    };

    Snapshot consume() {
        Snapshot snap;
        snap.userThdId     = _userThdId;
        snap.inflightCount = _inflightCount.load(std::memory_order_relaxed);
        snap.maxQueueDepth = _maxQueueDepth.exchange(0, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(_metricsMutex);
            snap.types.reserve(_metrics.size());

            for (auto& pair : _metrics) {
                TypeSnapshot ts;
                ts.requestType   = pair.first;
                ts.totalCount    = pair.second.totalCount.exchange(0, std::memory_order_relaxed);
                ts.totalReqBytes = pair.second.totalReqBytes.exchange(0, std::memory_order_relaxed);
                ts.totalRspBytes = pair.second.totalRspBytes.exchange(0, std::memory_order_relaxed);
                ts.histSnap      = pair.second.latencyHist.snapshot();
                snap.types.push_back(std::move(ts));
            }
        }
        return snap;
    }

private:
    struct PerTypeMetrics {
        Histogram latencyHist;
        std::atomic<int>     totalCount   {0};
        std::atomic<int64_t> totalReqBytes{0};
        std::atomic<int64_t> totalRspBytes{0};
    };

    PerTypeMetrics& getOrCreateMetrics(uint32_t type) {
        std::lock_guard<std::mutex> lock(_metricsMutex);
        return _metrics[type];
    }

    int                                    _userThdId;
    std::atomic<uint32_t>                  _inflightCount{0};
    std::atomic<uint32_t>                  _maxQueueDepth{0};
    std::unordered_map<uint32_t, long>     _reqStartTime;
    std::unordered_map<uint32_t, PerTypeMetrics> _metrics;
    std::mutex                             _metricsMutex;
};

class ReqStatLogger {
public:
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
        ReqStatLogger* self = static_cast<ReqStatLogger*>(param);

        std::vector<RequestStat*> allStat;
        {
            std::lock_guard<std::mutex> lock(self->_lockAllStat);
            allStat = self->_allStat;
        }

        for (RequestStat* reqStat : allStat) {
            RequestStat::Snapshot snap = reqStat->consume();

            for (auto& ts : snap.types) {
                StatBill bill;
                bill.userThdId     = snap.userThdId;
                bill.requestType   = ts.requestType;
                bill.totalCount    = ts.totalCount;
                bill.totalReqBytes = ts.totalReqBytes;
                bill.totalRspBytes = ts.totalRspBytes;
                bill.inflightCount = snap.inflightCount;
                bill.maxQueueDepth = snap.maxQueueDepth;

                if (ts.histSnap.count > 0) {
                    bill.avgCost = ts.histSnap.sum / ts.histSnap.count;
                    bill.maxCost = ts.histSnap.maxCost;
                    bill.minCost = ts.histSnap.minCost;
                    bill.p50 = Histogram::percentile(0.50, ts.histSnap.count, ts.histSnap.buckets, ts.histSnap.maxCost);
                    bill.p80 = Histogram::percentile(0.80, ts.histSnap.count, ts.histSnap.buckets, ts.histSnap.maxCost);
                    bill.p90 = Histogram::percentile(0.90, ts.histSnap.count, ts.histSnap.buckets, ts.histSnap.maxCost);
                    bill.p95 = Histogram::percentile(0.95, ts.histSnap.count, ts.histSnap.buckets, ts.histSnap.maxCost);
                    bill.p99 = Histogram::percentile(0.99, ts.histSnap.count, ts.histSnap.buckets, ts.histSnap.maxCost);
                }

                if (bill.totalCount > 0) {
                    self->logStatBill(bill);
                }
            }
        }
        return true;
    }

    void logStatBill(const StatBill& bill) {
        LOG_MSG(LogLevel::Info,
            "tid=%d, type=0x%x, cnt=%d, IF/queue=%u/%u, "
            "avg=%d, P50/P80/P90=%d/%d/%d, P95/P99=%d/%d, min/max=%d/%d, "
            "req/rsp=%ld/%ld",
            bill.userThdId, bill.requestType, bill.totalCount, bill.inflightCount, bill.maxQueueDepth,
            bill.avgCost, bill.p50, bill.p80, bill.p90, bill.p95, bill.p99, bill.minCost, bill.maxCost,
            bill.totalReqBytes, bill.totalRspBytes);
    }

private:
    int          _interval;
    timer_helper _tmHelper;
    std::vector<RequestStat*> _allStat;
    std::mutex                _lockAllStat;
};

}  // namespace zbf
