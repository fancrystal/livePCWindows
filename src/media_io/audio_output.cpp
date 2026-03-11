#include "media_io/audio_output.h"
#include "common/log.h"
#include "common/timestamp.h"
#include <cstring>

namespace live_assistant {

AudioOutput::AudioOutput() {
    LOG_INFO("[AudioOutput] Created");
}

AudioOutput::~AudioOutput() {
    shutdown();
}

bool AudioOutput::initialize(const AudioOutputConfig& config) {
    if (initialized_) {
        return true;
    }

    config_ = config;

    // 初始化混音缓冲区
    mix_buffers_.resize(config_.channels);
    for (auto& buf : mix_buffers_) {
        buf.resize(config_.frames_per_buffer, 0.0f);
    }

    initialized_ = true;
    LOG_INFO("[AudioOutput] Initialized: " + std::to_string(config_.sample_rate) + 
             "Hz, " + std::to_string(config_.channels) + "ch, " +
             std::to_string(config_.frames_per_buffer) + " frames/buffer");
    return true;
}

void AudioOutput::shutdown() {
    stop();
    initialized_ = false;
    LOG_INFO("[AudioOutput] Shutdown");
}

bool AudioOutput::start() {
    if (!initialized_ || running_) {
        return false;
    }

    running_ = true;
    start_time_ns_ = Timestamp::gettime_ns();
    total_samples_ = 0;

    audio_thread_ = std::thread(&AudioOutput::audio_thread, this);

    LOG_INFO("[AudioOutput] Started");
    return true;
}

void AudioOutput::stop() {
    if (!running_) {
        return;
    }

    running_ = false;
    if (audio_thread_.joinable()) {
        audio_thread_.join();
    }

    LOG_INFO("[AudioOutput] Stopped");
}

bool AudioOutput::is_running() const {
    return running_;
}

void AudioOutput::register_input(const std::string& source_id, AudioOutputCallback callback) {
    std::lock_guard<std::mutex> lock(inputs_mutex_);
    
    // 检查是否已存在
    for (auto& input : inputs_) {
        if (input.source_id == source_id) {
            input.callback = callback;
            LOG_INFO("[AudioOutput] Updated input: " + source_id);
            return;
        }
    }
    
    inputs_.push_back({source_id, callback});
    LOG_INFO("[AudioOutput] Registered input: " + source_id + " (total: " + std::to_string(inputs_.size()) + ")");
}

void AudioOutput::unregister_input(const std::string& source_id) {
    std::lock_guard<std::mutex> lock(inputs_mutex_);
    
    for (auto it = inputs_.begin(); it != inputs_.end(); ++it) {
        if (it->source_id == source_id) {
            inputs_.erase(it);
            LOG_INFO("[AudioOutput] Unregistered input: " + source_id);
            return;
        }
    }
}

void AudioOutput::set_output_callback(AudioOutputCallback callback) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    output_callback_ = callback;
}

// 音频线程主循环（照搬 OBS audio_thread）
void AudioOutput::audio_thread() {
    LOG_INFO("[AudioOutput] Audio thread started");

    uint64_t samples = 0;
    uint64_t prev_time = start_time_ns_;

    while (running_) {
        // 累加样本数
        samples += config_.frames_per_buffer;
        
        // 计算目标时间
        uint64_t audio_time = start_time_ns_ + Timestamp::samples_to_ns(samples, config_.sample_rate);

        // 高精度睡眠到目标时间
        Timestamp::sleep_to_ns(audio_time);

        // 处理输入和输出
        input_and_output(audio_time, prev_time);
        prev_time = audio_time;
    }

    LOG_INFO("[AudioOutput] Audio thread stopped");
}

// 输入和输出处理（照搬 OBS input_and_output）
void AudioOutput::input_and_output(uint64_t audio_time_ns, uint64_t prev_time_ns) {
    // 清空混音缓冲区
    for (auto& buf : mix_buffers_) {
        std::fill(buf.begin(), buf.end(), 0.0f);
    }

    // 收集所有输入源的音频数据
    std::vector<float*> input_data;
    {
        std::lock_guard<std::mutex> lock(inputs_mutex_);
        
        for (auto& input : inputs_) {
            if (input.callback) {
                // 这里简化处理，实际应该从音频源获取数据
                // 暂时跳过，由音频源主动推送
            }
        }
    }

    // 执行混音
    float* mix_ptrs[8] = {nullptr};  // 最多8通道
    for (size_t ch = 0; ch < config_.channels && ch < 8; ++ch) {
        mix_ptrs[ch] = mix_buffers_[ch].data();
    }

    // 输出到回调
    std::lock_guard<std::mutex> lock(output_mutex_);
    if (output_callback_) {
        output_callback_(audio_time_ns, 
                        const_cast<const float* const*>(mix_ptrs), 
                        config_.frames_per_buffer, 
                        config_.channels);
    }
}

// 执行混音
void AudioOutput::do_mixing(float** mix_buffers, uint32_t frames) {
    // 混音逻辑：简单相加后限幅
    for (uint32_t ch = 0; ch < config_.channels; ++ch) {
        for (uint32_t i = 0; i < frames; ++i) {
            float val = mix_buffers[ch][i];
            // 限幅到 -1.0 ~ 1.0
            if (val > 1.0f) val = 1.0f;
            if (val < -1.0f) val = -1.0f;
            mix_buffers[ch][i] = val;
        }
    }
}

} // namespace live_assistant
