#pragma once

#include <cstdint>
#include <vector>

namespace live_assistant {

// 音频重采样配置
struct AudioResamplerConfig {
    uint32_t src_sample_rate = 44100;
    uint32_t dst_sample_rate = 48000;
    uint32_t channels = 2;
    uint32_t src_bits = 16;  // 源采样位数
    uint32_t dst_bits = 16;  // 目标采样位数
};

/**
 * @brief 音频重采样器
 * 
 * 用于将不同采样率的音频转换为统一的 48kHz
 */
class AudioResampler {
public:
    AudioResampler();
    ~AudioResampler();

    // 初始化
    bool initialize(const AudioResamplerConfig& config);
    
    // 关闭
    void shutdown();

    // 重采样
    // 输入：源音频数据
    // 输出：重采样后的音频数据（48kHz, s16）
    std::vector<uint8_t> resample(const uint8_t* src_data, size_t src_size);

    // 获取配置
    const AudioResamplerConfig& get_config() const { return config_; }

private:
    AudioResamplerConfig config_;
    bool initialized_ = false;
    
    // 内部缓冲区
    std::vector<float> float_buffer_;
    
    // 简单的线性插值重采样
    std::vector<int16_t> resample_linear(const int16_t* src, size_t src_samples);
};

} // namespace live_assistant
