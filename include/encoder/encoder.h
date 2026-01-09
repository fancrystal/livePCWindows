#pragma once

#include <string>
#include <memory>
#include <vector>

#include "common/error.h"
#include "encoder/encoder_config.h"

namespace live_assistant {

// 前向声明
struct VideoFrame;
struct AudioFrame;
class AudioEncoder;
class VideoEncoder;

class Encoder {
public:
    Encoder();
    ~Encoder();
    
    // 使用配置初始化编码器
    ErrorCode initialize_video_encoder(const VideoEncoderConfig& config);
    ErrorCode initialize_audio_encoder(const AudioEncoderConfig& config);
    
    // 使用新配置重新初始化编码器
    ErrorCode reinitialize_video_encoder(const VideoEncoderConfig& config);
    ErrorCode reinitialize_audio_encoder(const AudioEncoderConfig& config);
    
    ErrorCode shutdown();
    
    // 编码帧
    ErrorCode encode_video_frame(const std::shared_ptr<VideoFrame>& frame, std::vector<uint8_t>& encoded_data);
    ErrorCode encode_audio_frame(const std::shared_ptr<AudioFrame>& frame, std::vector<uint8_t>& encoded_data);
    
    // 获取编码器配置
    const VideoEncoderConfig& get_video_config() const;
    const AudioEncoderConfig& get_audio_config() const;
    
    // 获取编码器参数
    int get_video_bitrate() const;
    int get_audio_bitrate() const;
    
    // 设置编码器参数（动态）
    ErrorCode set_video_bitrate(int bitrate);
    ErrorCode set_audio_bitrate(int bitrate);
    
private:
    // 视频编码器
    std::unique_ptr<VideoEncoder> video_encoder_;
    VideoEncoderConfig video_config_;
    bool video_encoder_initialized_ = false;
    
    // 音频编码器
    std::unique_ptr<AudioEncoder> audio_encoder_;
    AudioEncoderConfig audio_config_;
    bool audio_encoder_initialized_ = false;
};

} // namespace live_assistant
