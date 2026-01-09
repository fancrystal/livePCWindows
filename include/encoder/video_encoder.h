#pragma once

#include <string>
#include <memory>
#include <vector>

#include "common/error.h"
#include "encoder/encoder_config.h"

// 前向声明
namespace live_assistant {
struct VideoFrame;
}

namespace live_assistant {

// 视频编码器抽象类
class VideoEncoder {
public:
    virtual ~VideoEncoder() = default;
    
    // 使用配置初始化编码器
    virtual ErrorCode initialize(const VideoEncoderConfig& config) = 0;
    
    // 关闭编码器
    virtual ErrorCode shutdown() = 0;
    
    // 编码视频帧
    virtual ErrorCode encode(const std::shared_ptr<VideoFrame>& frame, std::vector<uint8_t>& encoded_data, bool& is_keyframe) = 0;
    
    // 获取编码器配置
    virtual const VideoEncoderConfig& get_config() const = 0;
    
    // 动态设置编码器比特率 (如果支持)
    virtual ErrorCode set_bitrate(int bitrate) = 0;
    
    // 获取当前比特率
    virtual int get_bitrate() const = 0;
    
    // 强制生成关键帧
    virtual ErrorCode force_keyframe() = 0;
};

// H.264视频编码器实现
class H264Encoder : public VideoEncoder {
public:
    H264Encoder();
    ~H264Encoder() override;
    
    ErrorCode initialize(const VideoEncoderConfig& config) override;
    ErrorCode shutdown() override;
    ErrorCode encode(const std::shared_ptr<VideoFrame>& frame, std::vector<uint8_t>& encoded_data, bool& is_keyframe) override;
    
    const VideoEncoderConfig& get_config() const override {
        return config_;
    }
    
    ErrorCode set_bitrate(int bitrate) override;
    int get_bitrate() const override {
        return config_.bitrate;
    }
    
    ErrorCode force_keyframe() override;
    
private:
    // 配置
    VideoEncoderConfig config_;
    
    // 编码器状态
    bool initialized_ = false;
    bool force_keyframe_ = false;
    
    // FFmpeg编码器组件
    void* codec_ = nullptr;
    void* codec_ctx_ = nullptr;
    void* frame_ = nullptr;
    void* pkt_ = nullptr;
    
    // 辅助方法
    std::string preset_to_string(VideoEncodingPreset preset) const;
};

} // namespace live_assistant
