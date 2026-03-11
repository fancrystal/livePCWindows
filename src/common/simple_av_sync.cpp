#include "common/simple_av_sync.h"
#include "common/log.h"
#include <chrono>

namespace live_assistant {

SimpleAVSync::SimpleAVSync() {
    LOG_INFO("[SimpleAVSync] Created");
}

SimpleAVSync::~SimpleAVSync() {
    stop();
}

void SimpleAVSync::start(uint32_t sample_rate, uint32_t fps) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    sample_rate_ = sample_rate;
    fps_ = fps;
    frame_duration_ms_ = 1000 / fps;
    
    // 🔧 修复：基准时间戳设为0，所有时间戳都相对于开始时间
    base_timestamp_ms_ = 0;
    
    total_audio_samples_ = 0;
    total_video_frames_ = 0;
    running_ = true;
    
    LOG_INFO("[SimpleAVSync] Started: sample_rate=" + std::to_string(sample_rate_) + 
             ", fps=" + std::to_string(fps_) + 
             ", base_timestamp=" + std::to_string(base_timestamp_ms_) + "ms");
}

void SimpleAVSync::stop() {
    running_ = false;
    LOG_INFO("[SimpleAVSync] Stopped");
}

bool SimpleAVSync::is_running() const {
    return running_;
}

int64_t SimpleAVSync::get_audio_timestamp_ms() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return 0;
    }
    
    // 🔧 修复：基于样本数计算时间戳，从0开始
    // 公式：samples * 1000 / sample_rate
    uint64_t samples = total_audio_samples_.load();
    int64_t timestamp_ms = (samples * 1000) / sample_rate_;
    return timestamp_ms;  // 从0开始，不需要加 base_timestamp
}

int64_t SimpleAVSync::get_video_timestamp_ms() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return 0;
    }
    
    // 🔧 修复：基于帧数计算时间戳，从0开始
    // 公式：frames * frame_duration
    uint64_t frames = total_video_frames_.load();
    int64_t timestamp_ms = frames * frame_duration_ms_;
    return timestamp_ms;  // 从0开始，不需要加 base_timestamp
}

void SimpleAVSync::add_audio_samples(uint32_t samples) {
    total_audio_samples_ += samples;
}

void SimpleAVSync::add_video_frame() {
    total_video_frames_++;
}

void SimpleAVSync::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 🔧 修复：重置后时间戳从0开始
    base_timestamp_ms_ = 0;
    total_audio_samples_ = 0;
    total_video_frames_ = 0;
    
    LOG_INFO("[SimpleAVSync] Reset: base_timestamp=" + std::to_string(base_timestamp_ms_) + "ms");
}

uint64_t SimpleAVSync::get_total_audio_samples() const {
    return total_audio_samples_.load();
}

uint64_t SimpleAVSync::get_total_video_frames() const {
    return total_video_frames_.load();
}

} // namespace live_assistant
