#pragma once

#include "common/error.h"
#include "stream_pusher/stream_config.h"
#include "stream_pusher/media_packet.h"

// FFmpeg类型的前向声明
struct AVFormatContext;
struct AVStream;
struct AVPacket;

namespace live_assistant {

// RTMP推流器实现
class RTMPPusher {
public:
    RTMPPusher();
    ~RTMPPusher();
    
    // 初始化RTMP推流器
    ErrorCode initialize(const StreamConfig& config);
    
    // 连接到RTMP服务器
    ErrorCode connect();
    
    // 断开与RTMP服务器的连接
    ErrorCode disconnect();
    
    // 发送媒体包
    ErrorCode send_packet(const MediaPacket& packet);
    
    // 是否已连接到服务器
    bool is_connected() const;
    
    // 获取当前统计信息
    struct Stats {
        bool connected;
        int audio_packets_sent;
        int video_packets_sent;
        int bytes_sent;
        int reconnect_attempts;
    };
    Stats get_stats() const;
    
    // 重置统计信息
    void reset_stats();
    
private:
    // 初始化AVFormatContext
    ErrorCode init_format_context();
    
    // 添加音频流
    ErrorCode add_audio_stream();
    
    // 添加视频流
    ErrorCode add_video_stream();
    
    // 发送配置包 (SPS/PPS/AAC配置)
    ErrorCode send_config_packet(const MediaPacket& packet);
    
    // 发送编码后的音频包
    ErrorCode send_audio_packet(const MediaPacket& packet);
    
    // 发送编码后的视频包
    ErrorCode send_video_packet(const MediaPacket& packet);
    
    // 释放FFmpeg资源
    void free_resources();
    
    // 推流配置
    StreamConfig config_;
    
    // FFmpeg格式上下文
    AVFormatContext* format_ctx_ = nullptr;
    
    // 音频和视频流
    AVStream* audio_stream_ = nullptr;
    AVStream* video_stream_ = nullptr;
    
    // 连接状态
    bool connected_ = false;
    
    // 统计信息
    Stats stats_ = {false, 0, 0, 0, 0};
};

} // namespace live_assistant