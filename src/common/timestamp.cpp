#include "common/timestamp.h"
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace live_assistant {

uint64_t Timestamp::gettime_ns() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
}

void Timestamp::sleep_to_ns(uint64_t target_time_ns) {
    uint64_t current = gettime_ns();
    if (target_time_ns > current) {
        uint64_t delay_ns = target_time_ns - current;
        auto delay_us = std::chrono::microseconds(delay_ns / 1000);
        
#ifdef _WIN32
        // Windows 高精度睡眠
        HANDLE timer = CreateWaitableTimer(NULL, TRUE, NULL);
        if (timer) {
            LARGE_INTEGER li;
            li.QuadPart = -static_cast<int64_t>(delay_ns / 100); // 100ns units
            SetWaitableTimer(timer, &li, 0, NULL, NULL, 0);
            WaitForSingleObject(timer, INFINITE);
            CloseHandle(timer);
        } else {
            std::this_thread::sleep_for(delay_us);
        }
#else
        std::this_thread::sleep_for(delay_us);
#endif
    }
}

} // namespace live_assistant
