#pragma once

#include <cstdint>
#include <atomic>
#include <mutex>

namespace live_assistant {

/**
 * @brief 简化的音视频同步系统
 * 
 * 基于 OBS 的核心思想：
 * 1. 音频时间戳基于样本数累加（固定周期）
 * 2. 视频时间戳与音频同步
 * 3. 使用统一的起始基准
 */
class SimpleAVSync {
public:
    SimpleAVSync();
    ~SimpleAVSync();

    // 启动同步系统
    void start(uint32_t sample_rate = 48000, uint32_t fps = 30);
    
    // 停止
    void stop();

    // 是否运行中
    bool is_running() const;

    // 获取音频时间戳（毫秒）- 基于样本数累加
    int64_t get_audio_timestamp_ms();

    // 获取视频时间戳（毫秒）- 与音频同步
    int64_t get_video_timestamp_ms();

    // 增加音频样本数（编码器调用）
    void add_audio_samples(uint32_t samples);

    // 增加视频帧计数
    void add_video_frame();

    // 重置
    void reset();

    // 获取统计信息
    uint64_t get_total_audio_samples() const;
    uint64_t get_total_video_frames() const;

private:
    std::atomic<bool> running_{false};
    
    // 配置
    uint32_t sample_rate_ = 48000;
    uint32_t fps_ = 30;
    int64_t frame_duration_ms_ = 33;  // 1000 / 30
    
    // 时间戳基准
    std::mutex mutex_;
    int64_t base_timestamp_ms_ = 0;
    
    // 累计计数
    std::atomic<uint64_t> total_audio_samples_{0};
    std::atomic<uint64_t> total_video_frames_{0};
};

} // namespace live_assistant
