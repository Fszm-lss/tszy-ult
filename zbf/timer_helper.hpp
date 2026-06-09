#ifndef timer_helper_hpp
#define timer_helper_hpp

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <list>
#include <unordered_map>
#include <functional>
#include "log_utils.hpp"

namespace zbf {

typedef bool (*TIMER_TASK)(void*);
typedef std::list<std::function<bool()> > TimerList;


enum TickUnit {   
    TenMilliSec, // 10MS | 20MS | 30MS | 50MS | 100MS | 200MS | 300MS | 500MS | 1000MS
    Second,      // 1S   | 2S   | 3S   | 5S   | 10S   | 15S   | 20S   | 30S   | 60S
    TenSecond,   // 10S  | 20S  | 30S  | 1M   | 1.5M  | 2M    | 3M    | 5M    | 10M
    Minute,      // 1M   | 2M   | 3M   | 5M   | 10M   | 15M   | 20M   | 30M   | 60M
};

class timer_helper {
public:
    timer_helper(TickUnit tickUnit) {
        _tickUnit = tickUnit;
    }

    ~timer_helper() {
    }

    int start(short thdidx = 0) {
        _thd = std::thread([&, thdidx](){
			timerProc(thdidx);
		});
        return 0;
    }

    // must stop() before destructor
    void stop() {
        {
            std::lock_guard<std::mutex> lock(_cvLock);
            _stopped.store(true, std::memory_order_release);
        }
        _cv.notify_one();
        if (_thd.joinable()) _thd.join();
    }

    void addTimerTask(int interval, TIMER_TASK timerTask, void* param) {
        _timerTasks[interval].push_back([timerTask, param]() { return timerTask(param); });
    }

private:
    void timerProc(short thdId) {
        log_utils::registerThreadId(thdId);
        LOG_MSG(LogLevel::Debug, "timer thread(%d) start", thdId);

        static int timeSlotTenMS[]  = { 1, 2, 3, 5, 10, 20, 30, 50, 100 }; // unit 10MS
        static int timeSlotSec[]    = { 1, 2, 3, 5, 10, 15, 20, 30, 60  }; // unit 1S
        static int timeSlotTenSec[] = { 1, 2, 3, 6,  9, 12, 18, 30, 60  }; // unit 10S
        static int timeSlotMin[]    = { 1, 2, 3, 5, 10, 15, 20, 30, 60  }; // unit 1M

        int* timeSlot = nullptr;
        int timeSlotSZ = 0;
        std::chrono::milliseconds waitMs;

        switch (_tickUnit) {
            case TickUnit::TenMilliSec: {
                timeSlot = timeSlotTenMS;
                timeSlotSZ = sizeof(timeSlotTenMS)/sizeof(int);
                waitMs = std::chrono::milliseconds(10);
                break;
            }
            case TickUnit::Second: {
                timeSlot = timeSlotSec;
                timeSlotSZ = sizeof(timeSlotSec)/sizeof(int);
                waitMs = std::chrono::seconds(1);
                break;
            }
            case TickUnit::TenSecond: {
                timeSlot = timeSlotTenSec;
                timeSlotSZ = sizeof(timeSlotTenSec)/sizeof(int);
                waitMs = std::chrono::seconds(10);
                break;
            }
            case TickUnit::Minute: {
                timeSlot = timeSlotMin;
                timeSlotSZ = sizeof(timeSlotMin)/sizeof(int);
                waitMs = std::chrono::seconds(60);
                break;
            }
            default:
                return;
        }
        
        int tick = 0; // 1 unit

        for (;;) {
            {
                std::unique_lock<std::mutex> lock(_cvLock);
                if (_cv.wait_for(lock, waitMs, [this]{ return _stopped.load(std::memory_order_acquire); })) break;
            }

            tick++;
            for (int i = 0; i < timeSlotSZ; ++i) {
                if (_stopped.load(std::memory_order_acquire)) break;
                if (tick % timeSlot[i] == 0) {
                    onTicks(timeSlot[i]);
                }
            }
            if (tick == timeSlot[timeSlotSZ-1]) tick = 0;
        }
        LOG_MSG(LogLevel::Debug, "timer thread(%d) exit", thdId);
    }

    void onTicks(int ticks) {
        TimerList& taskList = _timerTasks[ticks];
        size_t taskSize = taskList.size();
        for (int i = 0; i < taskSize; ++i) {
            if (_stopped.load(std::memory_order_acquire)) break;

            std::function<bool()> task = taskList.front();
            taskList.pop_front();
            if (task()) {
                taskList.push_back(task);
            }
        }
    }

private:
    std::thread _thd;
    TickUnit _tickUnit;
    std::unordered_map<unsigned short, TimerList> _timerTasks;
    std::condition_variable _cv;
    std::mutex _cvLock;
    std::atomic<bool> _stopped{false};
};


}

#endif
