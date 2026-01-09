#include "common/media_clock.h"

namespace live_assistant {

MediaClock::MediaClock() {
    reset();
}

MediaTimestamp MediaClock::now() const {
    return MediaTimestamp();
}

MediaTimeUs MediaClock::now_us() const {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

MediaTimestamp MediaClock::get_start_time() const {
    return MediaTimestamp(start_time_us_);
}

MediaTimeUs MediaClock::get_start_time_us() const {
    return start_time_us_;
}

void MediaClock::reset() {
    start_time_us_ = now_us();
    running_ = false;
}

bool MediaClock::is_running() const {
    return running_;
}

void MediaClock::start() {
    start_time_us_ = now_us();
    running_ = true;
}

void MediaClock::stop() {
    running_ = false;
}

MediaTimestamp MediaClock::get_elapsed_time() const {
    if (!running_) {
        return MediaTimestamp(0);
    }
    return MediaTimestamp(now_us() - start_time_us_);
}

MediaTimeUs MediaClock::get_elapsed_time_us() const {
    if (!running_) {
        return 0;
    }
    return now_us() - start_time_us_;
}

MediaClock& MediaClock::instance() {
    static MediaClock instance;
    return instance;
}

} // namespace live_assistant