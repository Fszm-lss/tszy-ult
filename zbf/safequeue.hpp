#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <list>
#include <mutex>
#include <thread>

namespace zbf {

enum SQ_PopCode { SQ_POP_SUCCESS = 0, SQ_TIMEOUT = 1, SQ_EXIT = 2 };

// FIFO
template<typename T, size_t Cap = 256, bool UseCV = true >
class SafeQueue {
public:
    bool push(const T& item) {
        bool ret = false;
        {
            std::lock_guard<std::mutex> lock(_mtx);
            if (_queue.size() < Cap) {
                _queue.push_back(item);
                ret = true;
            }
        }
        if (UseCV && ret) _cv.notify_one();
        return ret;
    }

    bool push(T&& item) {
        bool ret = false;
        {
            std::lock_guard<std::mutex> lock(_mtx);
            if (_queue.size() < Cap) {
                _queue.push_back(std::move(item));
                ret = true;
            }
        }
        if (UseCV && ret) _cv.notify_one();
        return ret;
    }

    int pop(T& item) {
        if (!UseCV) return pop_ncv(item);

        std::unique_lock<std::mutex> lock(_mtx);
        // use while to avoid fake-wakeup
        while (_queue.empty() && !_exit) {
            _cv.wait(lock);
        }
        if (_exit) return SQ_EXIT;
        item = std::move(_queue.front());
        _queue.pop_front();
        return SQ_POP_SUCCESS;
    }

    int pop_timeout(T& item, int timeout_ms = 100) {
        if (!UseCV) return pop_timeout_ncv(item, timeout_ms);

        std::unique_lock<std::mutex> lock(_mtx);
        bool ok = _cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this]() {
            return !_queue.empty() || _exit;
        });
        if (_exit) return SQ_EXIT;
        if (!ok) return SQ_TIMEOUT;
        item = std::move(_queue.front());
        _queue.pop_front();
        return SQ_POP_SUCCESS;
    }

    void requeue(T&& item) {
        std::lock_guard<std::mutex> lock(_mtx);
        _queue.push_front(std::move(item));
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(_mtx);
            _exit = true;
        }
        if (UseCV) _cv.notify_all();
    }

    bool is_stop() {
        std::lock_guard<std::mutex> lock(_mtx);
        return _exit;
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(_mtx);
        return _queue.size();
    }

    bool empty() {
        std::lock_guard<std::mutex> lock(_mtx);
        return _queue.empty();
    }

    bool raw_pop(T& item) {
        std::lock_guard<std::mutex> lock(_mtx);
        if (_queue.empty()) return false;
        item = std::move(_queue.front());
        _queue.pop_front();
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(_mtx);
        _queue.clear();
    }

private:
    int pop_ncv(T& item) {
        int rc = SQ_EXIT;
        while (true) {
            if (is_stop()) {
                rc = SQ_EXIT;
                break;
            }
            {
                std::lock_guard<std::mutex> lock(_mtx);
                if (!_queue.empty()) {
                    item = std::move(_queue.front());
                    _queue.pop_front();
                    rc = SQ_POP_SUCCESS;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(SLEEP_TIME));
        }
        return rc;
    }

    int pop_timeout_ncv(T& item, int timeout_ms = 100) {
        int rc = SQ_TIMEOUT;
        int sleep_times = std::max(1, timeout_ms / SLEEP_TIME);
        while (sleep_times > 0) {
            if (is_stop()) {
                rc = SQ_EXIT;
                break;
            }
            {
                std::lock_guard<std::mutex> lock(_mtx);
                if (!_queue.empty()) {
                    item = std::move(_queue.front());
                    _queue.pop_front();
                    rc = SQ_POP_SUCCESS;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(SLEEP_TIME));
            --sleep_times;
        }

        if (rc == SQ_TIMEOUT) {
            if (is_stop()) {
                rc = SQ_EXIT;
            } else {
                std::lock_guard<std::mutex> lock(_mtx);
                if (!_queue.empty()) {
                    item = std::move(_queue.front());
                    _queue.pop_front();
                    rc = SQ_POP_SUCCESS;
                }
            }
        }
        return rc;
    }

private:
    std::list<T>             _queue;
    std::mutex              _mtx;
    std::condition_variable _cv;
    bool                    _exit{false};
    enum { SLEEP_TIME = 5 };
};

}
