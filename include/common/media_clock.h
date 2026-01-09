#pragma once

#include <chrono>
#include <cstdint>
#include <atomic>
#include <string>

namespace live_assistant {

// Media time unit: microseconds
using MediaTimeUs = int64_t;

// Media time structure
template<typename Clock>
struct MediaTime {
    // Monotonic time point
    std::chrono::time_point<Clock> time_point;
    
    // Microseconds since epoch
    MediaTimeUs us;
    
    MediaTime() : us(0) {
        time_point = Clock::now();
        us = std::chrono::duration_cast<std::chrono::microseconds>(time_point.time_since_epoch()).count();
    }
    
    explicit MediaTime(MediaTimeUs us) : us(us) {
        time_point = Clock::time_point(std::chrono::microseconds(us));
    }
    
    MediaTime(const std::chrono::time_point<Clock>& tp) : time_point(tp) {
        us = std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
    }
    
    // Comparison operators
    bool operator<(const MediaTime& other) const {
        return us < other.us;
    }
    
    bool operator<=(const MediaTime& other) const {
        return us <= other.us;
    }
    
    bool operator>(const MediaTime& other) const {
        return us > other.us;
    }
    
    bool operator>=(const MediaTime& other) const {
        return us >= other.us;
    }
    
    bool operator==(const MediaTime& other) const {
        return us == other.us;
    }
    
    bool operator!=(const MediaTime& other) const {
        return us != other.us;
    }
    
    // Arithmetic operators
    MediaTime operator+(const MediaTime& other) const {
        return MediaTime(us + other.us);
    }
    
    MediaTime operator-(const MediaTime& other) const {
        return MediaTime(us - other.us);
    }
    
    MediaTime& operator+=(const MediaTime& other) {
        us += other.us;
        time_point = Clock::time_point(std::chrono::microseconds(us));
        return *this;
    }
    
    MediaTime& operator-=(const MediaTime& other) {
        us -= other.us;
        time_point = Clock::time_point(std::chrono::microseconds(us));
        return *this;
    }
    
    // Convert to string
    std::string to_string() const {
        return std::to_string(us);
    }
};

// Using steady_clock for monotonic time
typedef MediaTime<std::chrono::steady_clock> MediaTimestamp;

// Media clock class
class MediaClock {
public:
    MediaClock();
    ~MediaClock() = default;
    
    // Get current time as MediaTimestamp
    MediaTimestamp now() const;
    
    // Get current time in microseconds
    MediaTimeUs now_us() const;
    
    // Get clock start time
    MediaTimestamp get_start_time() const;
    
    // Get clock start time in microseconds
    MediaTimeUs get_start_time_us() const;
    
    // Reset clock (should only be called once at initialization)
    void reset();
    
    // Check if clock is running
    bool is_running() const;
    
    // Start clock (resets start time)
    void start();
    
    // Stop clock
    void stop();
    
    // Get elapsed time since start
    MediaTimestamp get_elapsed_time() const;
    
    // Get elapsed time since start in microseconds
    MediaTimeUs get_elapsed_time_us() const;
    
    // Singleton instance
    static MediaClock& instance();
    
private:
    // Start time of the clock
    std::atomic<MediaTimeUs> start_time_us_;
    
    // Is clock running
    std::atomic<bool> running_;
};

} // namespace live_assistant