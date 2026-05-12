#include "audio_engine/audio_mixer.h"
#include "common/log.h"
#include <cstring>
#include <algorithm>

namespace live_assistant {

AudioMixer::AudioMixer() {
    LOG_INFO("[AudioMixer] Created");
}

AudioMixer::~AudioMixer() {
    shutdown();
    LOG_INFO("[AudioMixer] Destroyed");
}

bool AudioMixer::initialize(int sample_rate, int channels) {
    if (initialized_) {
        LOG_WARNING("[AudioMixer] Already initialized");
        return true;
    }

    sample_rate_ = sample_rate;
    channels_ = channels;
    frame_samples_ = 1024;  // AAC标准帧大小

    running_ = true;
    mixer_thread_ = std::thread(&AudioMixer::mixer_thread_func, this);

    initialized_ = true;
    LOG_INFO("[AudioMixer] Initialized: sample_rate=" + std::to_string(sample_rate_) +
             ", channels=" + std::to_string(channels_) +
             ", frame_samples=" + std::to_string(frame_samples_));
    return true;
}

void AudioMixer::shutdown() {
    if (!initialized_) {
        return;
    }

    running_ = false;
    if (mixer_thread_.joinable()) {
        mixer_thread_.join();
    }

    std::lock_guard<std::mutex> lock(queues_mutex_);
    input_queues_.clear();

    std::lock_guard<std::mutex> lock2(config_mutex_);
    input_configs_.clear();

    initialized_ = false;
    LOG_INFO("[AudioMixer] Shutdown complete");
}

bool AudioMixer::add_input(const AudioInputConfig& config) {
    std::lock_guard<std::mutex> lock(config_mutex_);

    if (input_configs_.find(config.id) != input_configs_.end()) {
        LOG_WARNING("[AudioMixer] Input already exists: " + config.id);
        return false;
    }

    input_configs_[config.id] = config;

    {
        std::lock_guard<std::mutex> lock(queues_mutex_);
        input_queues_[config.id] = std::make_shared<InputQueue>();
    }

    LOG_INFO("[AudioMixer] Added input: " + config.id + " (" + config.name + ")");
    return true;
}

void AudioMixer::remove_input(const std::string& input_id) {
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        input_configs_.erase(input_id);
    }

    {
        std::lock_guard<std::mutex> lock(queues_mutex_);
        input_queues_.erase(input_id);
    }

    LOG_INFO("[AudioMixer] Removed input: " + input_id);
}

bool AudioMixer::has_input(const std::string& input_id) const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return input_configs_.find(input_id) != input_configs_.end();
}

void AudioMixer::set_input_volume(const std::string& input_id, float volume) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    auto it = input_configs_.find(input_id);
    if (it != input_configs_.end()) {
        it->second.volume = std::max(0.0f, std::min(1.0f, volume));
        LOG_DEBUG("[AudioMixer] Set volume for " + input_id + ": " + std::to_string(volume));
    }
}

float AudioMixer::get_input_volume(const std::string& input_id) const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    auto it = input_configs_.find(input_id);
    if (it != input_configs_.end()) {
        return it->second.volume;
    }
    return 1.0f;
}

void AudioMixer::set_input_muted(const std::string& input_id, bool muted) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    auto it = input_configs_.find(input_id);
    if (it != input_configs_.end()) {
        it->second.muted = muted;
        LOG_DEBUG("[AudioMixer] Set muted for " + input_id + ": " + std::to_string(muted));
    }
}

bool AudioMixer::is_input_muted(const std::string& input_id) const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    auto it = input_configs_.find(input_id);
    if (it != input_configs_.end()) {
        return it->second.muted;
    }
    return false;
}

bool AudioMixer::submit_audio(const std::string& input_id, const QByteArray& data, int64_t timestamp_ms) {
    std::shared_ptr<InputQueue> queue;

    {
        std::lock_guard<std::mutex> lock(queues_mutex_);
        auto it = input_queues_.find(input_id);
        if (it == input_queues_.end()) {
            LOG_WARNING("[AudioMixer] Input not found: " + input_id);
            return false;
        }
        queue = it->second;
    }

    AudioFrameData frame;
    frame.data = data;
    frame.timestamp_ms = timestamp_ms;
    frame.sample_rate = sample_rate_;
    frame.channels = channels_;
    frame.samples = data.size() / (sizeof(int16_t) * channels_);

    std::lock_guard<std::mutex> lock(queue->mutex);
    if (queue->queue.size() < 10) {
        queue->queue.push(std::move(frame));
        return true;
    } else {
        LOG_WARNING("[AudioMixer] Input queue full, dropping frame: " + input_id);
        return false;
    }
}

void AudioMixer::set_mixed_audio_callback(MixedAudioCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    mixed_audio_callback_ = callback;
}

std::vector<AudioInputConfig> AudioMixer::get_inputs() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    std::vector<AudioInputConfig> inputs;
    for (const auto& pair : input_configs_) {
        inputs.push_back(pair.second);
    }
    return inputs;
}

void AudioMixer::set_noise_suppression_enabled(bool enabled) {
    noise_suppression_enabled_ = enabled;
    LOG_INFO("[AudioMixer] Noise suppression " + std::string(enabled ? "enabled" : "disabled"));
}

bool AudioMixer::is_noise_suppression_enabled() const {
    return noise_suppression_enabled_;
}

void AudioMixer::set_noise_suppression_level(float level) {
    noise_suppression_level_ = std::max(0.0f, std::min(1.0f, level));
    noise_gate_threshold_ = static_cast<int16_t>(noise_suppression_level_ * 2000);
    LOG_INFO("[AudioMixer] Noise suppression level: " + std::to_string(noise_suppression_level_));
}

void AudioMixer::mixer_thread_func() {
    LOG_INFO("[AudioMixer] Mixer thread started");

    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        do_mixing();
    }

    LOG_INFO("[AudioMixer] Mixer thread stopped");
}

void AudioMixer::do_mixing() {
    // Get total_inputs first, under config_mutex_ only (no nesting with queues_mutex_).
    size_t total_inputs = 0;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        total_inputs = input_configs_.size();
    }

    if (total_inputs == 0) return;

    std::vector<QByteArray> input_datas;
    int64_t earliest_timestamp = INT64_MAX;

    // Single critical section under queues_mutex_ for both the readiness check and the pop.
    // This eliminates the second loop that re-acquired queues_mutex_ inside the iteration,
    // which constituted a redundant (and potentially deadlock-prone) double lock pattern.
    {
        std::lock_guard<std::mutex> lock(queues_mutex_);

        if (input_queues_.size() != total_inputs) return;

        // Phase 1: check that every source has at least one frame ready.
        bool all_ready = true;
        for (const auto& pair : input_queues_) {
            std::lock_guard<std::mutex> q_lock(pair.second->mutex);
            if (pair.second->queue.empty()) {
                all_ready = false;
                break;
            }
            if (pair.second->queue.front().timestamp_ms < earliest_timestamp) {
                earliest_timestamp = pair.second->queue.front().timestamp_ms;
            }
        }

        if (!all_ready) return;

        // Phase 2: collect and pop one frame from each source.
        for (auto& pair : input_queues_) {
            std::lock_guard<std::mutex> q_lock(pair.second->mutex);
            if (!pair.second->queue.empty()) {
                input_datas.push_back(pair.second->queue.front().data);
                pair.second->queue.pop();
            }
        }
    }

    if (input_datas.empty()) return;

    // 执行混音（在锁外进行，不阻塞提交线程）
    QByteArray mixed = mix_audio_data(input_datas);

    AudioFrameData output;
    output.data = std::move(mixed);
    output.timestamp_ms = earliest_timestamp;
    output.sample_rate = sample_rate_;
    output.channels = channels_;
    output.samples = output.data.size() / (sizeof(int16_t) * channels_);

    MixedAudioCallback callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = mixed_audio_callback_;
    }

    if (callback) {
        callback(output);
    }
}

QByteArray AudioMixer::mix_audio_data(const std::vector<QByteArray>& inputs) {
    if (inputs.empty()) {
        return QByteArray();
    }

    // 获取所有输入的样本数（假设相同）
    int samples = inputs[0].size() / (sizeof(int16_t) * channels_);
    int bytes_per_sample = sizeof(int16_t) * channels_;

    QByteArray output(samples * bytes_per_sample, 0);

    // 遍历每个输入源
    for (size_t i = 0; i < inputs.size(); i++) {
        const auto& input = inputs[i];
        if (input.size() != samples * bytes_per_sample) {
            continue;  // 跳过不匹配的输入
        }

        // 获取音量
        float volume = 1.0f;
        bool muted = false;

        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            if (i < input_configs_.size()) {
                auto it = std::next(input_configs_.begin(), i);
                volume = it->second.volume;
                muted = it->second.muted;
            }
        }

        if (muted) {
            continue;  // 跳过静音的输入
        }

        // 🔧 降噪处理：对输入应用噪声门
        QByteArray processed_input = input;
        if (noise_suppression_enabled_) {
            int16_t* ptr = reinterpret_cast<int16_t*>(processed_input.data());
            for (int s = 0; s < samples * channels_; s++) {
                if (std::abs(ptr[s]) < noise_gate_threshold_) {
                    ptr[s] = 0;  // 低于阈值的样本设为0
                }
            }
        }

        // 混合
        const int16_t* in_ptr = reinterpret_cast<const int16_t*>(processed_input.constData());
        int16_t* out_ptr = reinterpret_cast<int16_t*>(output.data());

        for (int s = 0; s < samples * channels_; s++) {
            int32_t sum = out_ptr[s] + static_cast<int32_t>(in_ptr[s] * volume);
            out_ptr[s] = clamp_sample(sum);
        }
    }

    return output;
}

int16_t AudioMixer::clamp_sample(int32_t sample) {
    if (sample > INT16_MAX) {
        return INT16_MAX;
    }
    if (sample < INT16_MIN) {
        return INT16_MIN;
    }
    return static_cast<int16_t>(sample);
}

} // namespace live_assistant
