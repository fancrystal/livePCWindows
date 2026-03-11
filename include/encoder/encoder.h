#pragma once

#include <string>
#include <memory>
#include <vector>

#include "common/error.h"
#include "encoder/encoder_config.h"
#include "stream_pusher/encoded_packet.h"

struct AVCodecParameters;
struct AVRational;

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
    
    // 编码帧（方案2：输出 0..N 个编码包）
    ErrorCode encode_video_frame(const std::shared_ptr<VideoFrame>& frame, std::vector<EncodedPacketPtr>& packets);
    ErrorCode encode_audio_frame(const std::shared_ptr<AudioFrame>& frame, std::vector<EncodedPacketPtr>& packets);
    
    // 获取编码器配置
    const VideoEncoderConfig& get_video_config() const;
    const AudioEncoderConfig& get_audio_config() const;

    // 获取编码器参数（初始化后可用）
    AVCodecParameters* get_video_codec_parameters() const;
    AVRational get_video_time_base() const;
    AVCodecParameters* get_audio_codec_parameters() const;
    AVRational get_audio_time_base() const;
    
    // 获取编码器参数
    int get_video_bitrate() const;
    int get_audio_bitrate() const;
    
    // 设置编码器参数（动态）
    ErrorCode set_video_bitrate(int bitrate);
    ErrorCode set_audio_bitrate(int bitrate);
    
    // Force the next video frame to be a keyframe (IDR)
    ErrorCode force_keyframe();

    ErrorCode reset_audio_encoder();
    ErrorCode reset_video_encoder();

    // 获取音频编码器（用于直接连接信号）
    AudioEncoder* get_audio_encoder() const;

private:
    std::unique_ptr<VideoEncoder> video_encoder_;
    VideoEncoderConfig video_config_;
    bool video_encoder_initialized_ = false;
    
    std::unique_ptr<AudioEncoder> audio_encoder_;
    AudioEncoderConfig audio_config_;
    bool audio_encoder_initialized_ = false;
};

} // namespace live_assistant
