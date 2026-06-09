#pragma once

#include <mutex>
#include "socket_utils.hpp"

namespace zbf {

class heartbeat_helper {
public:
    heartbeat_helper(TimeUnitSec threshold) : _threshold(threshold), _lastTime(0) {
    }

    void update() {
        std::lock_guard<std::mutex> lock(_lockLT);
        _lastTime = socket_utils::currentTimeSecs();
    }

    bool isExceed() {
        TimeUnitSec now = socket_utils::currentTimeSecs();
        return isExceed(now);
    }

    bool isExceed(TimeUnitSec now) {
        std::lock_guard<std::mutex> lock(_lockLT);
        return (now - _lastTime) >= _threshold;
    }

private:
    TimeUnitSec _threshold;
    TimeUnitSec _lastTime;
    std::mutex  _lockLT;
};

}
