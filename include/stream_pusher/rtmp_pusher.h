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

        // 计算出的实时统计信息
        double bandwidth_kbps;        // 带宽 (kb/s)
        double video_fps;             // 视频帧率
        double audio_packets_per_sec; // 音频包发送速率
        int64_t total_bytes_sent;     // 总发送字节数
        std::chrono::steady_clock::time_point start_time; // 开始时间
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
    
    Stats stats_ = {false, 0, 0, 0, 0, 0.0, 0.0, 0.0, 0, std::chrono::steady_clock::now()};

    // Reconnect/backoff configuration
    int max_reconnect_attempts = 6;
    int base_backoff_ms = 500; // initial backoff in ms

    // Metrics helpers
    mutable std::mutex stats_mutex_;
    int64_t last_bytes_snapshot_ = 0;
    std::chrono::steady_clock::time_point last_snapshot_time_;

    // 滑动窗口统计（最近 1 秒的实时统计）
    mutable std::chrono::steady_clock::time_point window_start_time_;
    mutable int video_packets_in_window_ = 0;   // 窗口内视频包数
    mutable int audio_packets_in_window_ = 0;   // 窗口内音频包数
    mutable int64_t bytes_in_window_ = 0;       // 窗口内字节数
    mutable double last_calculated_fps_ = 0.0;       // 上次计算的 FPS
    mutable double last_calculated_bitrate_ = 0.0;   // 上次计算的码率 (kbps)
    
    // Allow stopping reconnect loops if destructor runs
    std::atomic<bool> stop_reconnect_{false};
    
    // Whether we've sent the first video keyframe yet
    bool have_sent_first_key_ = false;

    // send_packet 诊断计数（成员变量，重连时可重置）
    int send_frame_count_ = 0;
    int64_t av_sync_offset_ms_ = 0;            // 音视频同步偏移量：第一个实际发出的音频包 PTS
    int64_t video_pts_base_ = -1;              // 视频 PTS 归一化基准（修正 QSV 内部计数器偏移）
    int audio_packet_count_ = 0;
    int write_frame_count_ = 0;

    // 第一帧关键帧等待状态
    bool first_video_wait_initialized_ = false;
    std::chrono::steady_clock::time_point first_video_wait_start_;

    // 缓存的流参数，用于重连时自动重新注册流
    AVCodecParameters* cached_audio_codecpar_ = nullptr;
    AVCodecParameters* cached_video_codecpar_ = nullptr;
    AVRational cached_audio_time_base_ = {1, 1000};
    AVRational cached_video_time_base_ = {1, 1000};

    // 重新注册缓存的流（断线重连时使用）
    ErrorCode re_register_cached_streams();
};

} // namespace live_assistant
