#pragma once

#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "asio.hpp"
#include "log_utils.hpp"

namespace zbf {

class asio_worker {
public:
    using work_guard  = asio::executor_work_guard<asio::io_context::executor_type>;
    using work_strand = asio::strand<asio::io_context::executor_type>;

    asio_worker(const std::string& name, int num = 1) : _name(name), _workerctxs(num), _count(num), _worker_cursor(0), _strand_cursor(0) {
        for (int i = 0; i < _count; ++i) {
            _strands.emplace_back(asio::make_strand(_workerctxs[i]));
            _guards.emplace_back(asio::make_work_guard(_workerctxs[i]));
        }
    }

    void start() {
        // one io_context per thread
        for (int i = 0; i < _count; ++i) {
            _threads.emplace_back([this, i]() {
                LOG_MSG(LogLevel::Debug, "asio_worker[%s] thread(%d) start", _name.c_str(), i);
                _workerctxs[i].run();
                LOG_MSG(LogLevel::Debug, "asio_worker[%s] thread(%d) exit", _name.c_str(), i);
            });
        }
    }

    void stop() {
        // once the work object is destroyed, the service will stop
        for (auto& guard: _guards) {
            guard.reset();
        }        
    }

    void join() {
        for (auto& thd: _threads) {
            thd.join();
        }
    }

    // asio::io_context& get() {
    //     std::lock_guard<std::mutex> lock(_lockworker);
    //     if (_worker_cursor == _count) _worker_cursor = 0;
    //     return _workerctxs[_worker_cursor++];
    // }

    asio::io_context& get(int i) {
        i = i % _count;
        return _workerctxs[i];
    }

    asio_worker::work_strand& get_strand() {
        std::lock_guard<std::mutex> lock(_lockworker);
        if (_strand_cursor == _count) _strand_cursor = 0;
        return _strands[_strand_cursor++];
    }

    asio_worker::work_strand& get_strand(int i) {
        i = i % _count;
        return _strands[i];
    }

private:
    std::string                              _name;
    std::vector<asio::io_context>            _workerctxs;
    std::vector<work_strand>                 _strands;
    std::vector<work_guard>                  _guards;
    std::vector<std::thread>                 _threads;
    int                                      _count;
    int                                      _worker_cursor;
    int                                      _strand_cursor;
    std::mutex                               _lockworker;
};

}
