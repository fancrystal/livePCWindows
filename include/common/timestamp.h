#pragma once

#include <cstdint>

namespace live_assistant {

// 时间戳转换工具（照搬 OBS）
class Timestamp {
public:
    // 将样本数转换为纳秒
    static inline uint64_t samples_to_ns(uint64_t samples, uint32_t sample_rate) {
        return (samples * 1000000000ULL) / sample_rate;
    }

    // 将纳秒转换为样本数
    static inline uint64_t ns_to_samples(uint64_t ns, uint32_t sample_rate) {
        return (ns * sample_rate) / 1000000000ULL;
    }

    // 将毫秒转换为样本数
    static inline uint64_t ms_to_samples(uint64_t ms, uint32_t sample_rate) {
        return (ms * sample_rate) / 1000ULL;
    }

    // 将样本数转换为毫秒
    static inline uint64_t samples_to_ms(uint64_t samples, uint32_t sample_rate) {
        return (samples * 1000ULL) / sample_rate;
    }

    // 获取当前时间（纳秒）
    static uint64_t gettime_ns();

    // 高精度睡眠到指定时间
    static void sleep_to_ns(uint64_t target_time_ns);
};

} // namespace live_assistant
