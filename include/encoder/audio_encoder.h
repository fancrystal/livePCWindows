#pragma once

#include <string>
#include <memory>
#include <vector>

#include "common/error.h"
#include "encoder/encoder_config.h"

// 前向声明
namespace live_assistant {
struct AudioFrame;
}

namespace live_assistant {

// 音频编码器抽象类
class AudioEncoder {
public:
    virtual ~AudioEncoder() = default;
    
    // 使用配置初始化编码器
    virtual ErrorCode initialize(const AudioEncoderConfig& config) = 0;
    
    // 关闭编码器
    virtual ErrorCode shutdown() = 0;
    
    // 编码音频帧
    virtual ErrorCode encode(const std::shared_ptr<AudioFrame>& frame, std::vector<uint8_t>& encoded_data) = 0;
    
    // 获取编码器配置
    virtual const AudioEncoderConfig& get_config() const = 0;
    
    // 动态设置编码器比特率 (如果支持)
    virtual ErrorCode set_bitrate(int bitrate) = 0;
    
    // 获取当前比特率
    virtual int get_bitrate() const = 0;
};

// Opus音频编码器实现
class OpusEncoder : public AudioEncoder {
public:
    OpusEncoder();
    ~OpusEncoder() override;
    
    ErrorCode initialize(const AudioEncoderConfig& config) override;
    ErrorCode shutdown() override;
    ErrorCode encode(const std::shared_ptr<AudioFrame>& frame, std::vector<uint8_t>& encoded_data) override;
    
    const AudioEncoderConfig& get_config() const override {
        return config_;
    }
    
    ErrorCode set_bitrate(int bitrate) override;
    int get_bitrate() const override {
        return config_.bitrate;
    }
    
private:
    // 配置
    AudioEncoderConfig config_;
    
    // 编码器状态
    bool initialized_ = false;
    
    // Opus编码器实例
    void* encoder_ = nullptr;
};

// AAC音频编码器实现
class AACEncoder : public AudioEncoder {
public:
    AACEncoder();
    ~AACEncoder() override;
    
    ErrorCode initialize(const AudioEncoderConfig& config) override;
    ErrorCode shutdown() override;
    ErrorCode encode(const std::shared_ptr<AudioFrame>& frame, std::vector<uint8_t>& encoded_data) override;
    
    const AudioEncoderConfig& get_config() const override {
        return config_;
    }
    
    ErrorCode set_bitrate(int bitrate) override;
    int get_bitrate() const override {
        return config_.bitrate;
    }
    
private:
    // 配置
    AudioEncoderConfig config_;
    
    // 编码器状态
    bool initialized_ = false;
    
    // FFmpeg编码器组件
    void* codec_ = nullptr;
    void* codec_ctx_ = nullptr;
    void* frame_ = nullptr;
    void* pkt_ = nullptr;
};

} // namespace live_assistant
