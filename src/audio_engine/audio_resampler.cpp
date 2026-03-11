#include "audio_engine/audio_resampler.h"
#include "common/log.h"
#include <cstring>
#include <cmath>

namespace live_assistant {

AudioResampler::AudioResampler() {
    LOG_INFO("[AudioResampler] Created");
}

AudioResampler::~AudioResampler() {
    shutdown();
}

bool AudioResampler::initialize(const AudioResamplerConfig& config) {
    config_ = config;
    initialized_ = true;
    
    LOG_INFO("[AudioResampler] Initialized: " + 
             std::to_string(config_.src_sample_rate) + "Hz -> " +
             std::to_string(config_.dst_sample_rate) + "Hz, " +
             std::to_string(config_.channels) + "ch");
    return true;
}

void AudioResampler::shutdown() {
    initialized_ = false;
    LOG_INFO("[AudioResampler] Shutdown");
}

std::vector<uint8_t> AudioResampler::resample(const uint8_t* src_data, size_t src_size) {
    if (!initialized_ || !src_data || src_size == 0) {
        return {};
    }
    
    // 如果采样率相同，直接返回
    if (config_.src_sample_rate == config_.dst_sample_rate) {
        return std::vector<uint8_t>(src_data, src_data + src_size);
    }
    
    // 计算样本数
    size_t src_samples = src_size / sizeof(int16_t);
    const int16_t* src = reinterpret_cast<const int16_t*>(src_data);
    
    // 重采样
    std::vector<int16_t> dst = resample_linear(src, src_samples);
    
    // 转换为字节数组
    std::vector<uint8_t> result;
    result.resize(dst.size() * sizeof(int16_t));
    std::memcpy(result.data(), dst.data(), result.size());
    
    return result;
}

std::vector<int16_t> AudioResampler::resample_linear(const int16_t* src, size_t src_samples) {
    // 计算目标样本数
    double ratio = static_cast<double>(config_.dst_sample_rate) / config_.src_sample_rate;
    size_t dst_samples = static_cast<size_t>(src_samples * ratio);
    
    std::vector<int16_t> dst;
    dst.reserve(dst_samples);
    
    // 线性插值重采样
    for (size_t i = 0; i < dst_samples; ++i) {
        double src_pos = i / ratio;
        size_t src_idx = static_cast<size_t>(src_pos);
        double frac = src_pos - src_idx;
        
        // 边界检查
        if (src_idx >= src_samples - 1) {
            dst.push_back(src[src_samples - 1]);
        } else {
            // 线性插值
            int16_t s1 = src[src_idx];
            int16_t s2 = src[src_idx + 1];
            int16_t out = static_cast<int16_t>(s1 * (1.0 - frac) + s2 * frac);
            dst.push_back(out);
        }
    }
    
    return dst;
}

} // namespace live_assistant
