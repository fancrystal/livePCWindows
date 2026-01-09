#pragma once

#include <vector>
#include <cstdint>
#include "common/media_clock.h"

namespace live_assistant {

// 媒体包类型
enum class MediaType {
    AUDIO,
    VIDEO
};

// 视频帧类型
enum class VideoFrameType {
    I_FRAME,
    P_FRAME,
    B_FRAME
};

// 媒体包结构体
struct MediaPacket {
    // 媒体类型 (音频或视频)
    MediaType type;
    
    // 视频帧类型 (仅适用于视频包)
    VideoFrameType video_type = VideoFrameType::P_FRAME;
    
    // 媒体时间戳 (微秒)
    MediaTimeUs timestamp;
    
    // 编码数据
    std::vector<uint8_t> data;
    
    // 是否为关键帧 (仅视频)
    bool is_keyframe = false;
    
    // 是否为配置包 (SPS/PPS/AAC配置)
    bool is_config = false;
    
    // 优先级 (数值越大优先级越高)
    int priority = 0;
    
    // 默认构造函数
    MediaPacket() = default;
    
    // 不同媒体类型的构造函数
    MediaPacket(MediaType type, MediaTimeUs timestamp, const std::vector<uint8_t>& data) 
        : type(type), timestamp(timestamp), data(data) {
        // 设置优先级: 音频优先级高于视频
        if (type == MediaType::AUDIO) {
            priority = 2;
        } else {
            priority = 1;
        }
    }
    
    // 带帧类型的视频包构造函数
    MediaPacket(MediaTimeUs timestamp, const std::vector<uint8_t>& data, VideoFrameType video_type, bool is_keyframe) 
        : type(MediaType::VIDEO), 
          video_type(video_type), 
          timestamp(timestamp), 
          data(data), 
          is_keyframe(is_keyframe),
          priority(1) {}
    
    // 音频包构造函数
    MediaPacket(MediaTimeUs timestamp, const std::vector<uint8_t>& data) 
        : type(MediaType::AUDIO), 
          timestamp(timestamp), 
          data(data),
          priority(2) {}
};

} // namespace live_assistant