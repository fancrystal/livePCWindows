#pragma once

#include "common/error.h"
#include "stream_pusher/stream_config.h"
#include "stream_pusher/encoded_packet.h"
// STL
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdint>

// FFmpeg类型的前向声明
struct AVFormatContext;
struct AVStream;
struct AVCodecParameters;
struct AVRational;

namespace live_assistant {

// RTMP推流器实现（方案2：接收 codecpar + AVPacket）
class RTMPPusher {
public:
    RTMPPusher();
    ~RTMPPusher();
    
    // 初始化RTMP推流器（创建 output context 等）
    ErrorCode initialize(const StreamConfig& config);
    
    // 注册音频/视频流（必须在 connect_and_write_header 前调用）
    ErrorCode register_audio_stream(AVCodecParameters* codecpar, AVRational time_base);
    ErrorCode register_video_stream(AVCodecParameters* codecpar, AVRational time_base);

    // 连接到RTMP服务器并写 header
    ErrorCode connect_and_write_header();
    
    // 断开与RTMP服务器的连接
    ErrorCode disconnect();
    
    // 发送编码包
    ErrorCode send_packet(const EncodedPacketPtr& packet);
    
    // 是否已连接到服务器
    bool is_connected() const;
    
    // 是否已初始化（format context 已创建）
    bool is_initialized() const;
    
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
    ErrorCode init_format_context();
    ErrorCode open_output();
    
    void free_resources();
    
    StreamConfig config_;
    
    AVFormatContext* format_ctx_ = nullptr;
    AVStream* audio_stream_ = nullptr;
    AVStream* video_stream_ = nullptr;
    
    bool header_written_ = false;
    bool connected_ = false;
    
    Stats stats_ = {false, 0, 0, 0, 0};
    
    // Reconnect/backoff configuration
    int max_reconnect_attempts = 6;
    int base_backoff_ms = 500; // initial backoff in ms
    
    // Metrics helpers
    mutable std::mutex stats_mutex_;
    int64_t last_bytes_snapshot_ = 0;
    std::chrono::steady_clock::time_point last_snapshot_time_;
    
    // Allow stopping reconnect loops if destructor runs
    std::atomic<bool> stop_reconnect_{false};
    
    // Force next video packet to be treated as a keyframe (used after header write)
    std::atomic<bool> force_next_keyframe_{false};
    
    // Whether we've sent the first video keyframe yet
    bool have_sent_first_key_ = false;
};

} // namespace live_assistant
