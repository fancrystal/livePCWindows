#pragma once

#include <memory>

#include "common/error.h"
#include "encoder/encoder_config.h"
#include "encoder/audio_encoder.h"
#include "encoder/video_encoder.h"

namespace live_assistant {

// 编码器工厂类
class EncoderFactory {
public:
    EncoderFactory() = delete;
    ~EncoderFactory() = delete;
    
    // 根据配置创建音频编码器
    static std::unique_ptr<AudioEncoder> create_audio_encoder(const AudioEncoderConfig& config);
    
    // 根据配置创建视频编码器
    static std::unique_ptr<VideoEncoder> create_video_encoder(const VideoEncoderConfig& config);
};

} // namespace live_assistant
