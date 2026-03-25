#pragma once

#include <thread>
#include <atomic>
#include "common/error.h"
#include "stream_pusher/stream_config.h"
#include "stream_pusher/push_queue.h"
#include "stream_pusher/rtmp_pusher.h"

namespace live_assistant {

// 推流器状态
enum class StreamState {
    IDLE,
    CONNECTING,
    PUSHING,
    STOPPING,
    ERR
};

// 推流器类
class StreamPusher {
public:
    StreamPusher();
    ~StreamPusher();
    
    // 设置推流配置
    ErrorCode set_config(const StreamConfig& config);
    
    // 开始推流
    ErrorCode start();
    
    // 停止推流
    ErrorCode stop();
    
    // 注册编码流（必须在 start 前调用）
    ErrorCode register_audio_stream(AVCodecParameters* codecpar, AVRational time_base);
    ErrorCode register_video_stream(AVCodecParameters* codecpar, AVRational time_base);

    // 发送编码包到推流（线程安全入队）
    ErrorCode push_packet(EncodedPacketPtr packet);

    // 清空推流队列（用于reset前清空旧包）
    void clear_queue();

    // 获取当前推流状态
    StreamState get_state() const;
    
    // 推流是否正在进行
    bool is_pushing() const;
    
    // 推流是否处于错误状态
    bool is_in_error() const;
    
    // 获取当前统计信息
    struct Stats {
        StreamState state;
        bool connected;
        int audio_packets_sent;
        int video_packets_sent;
        int discarded_packets;
        int reconnect_attempts;

        // 计算出的实时统计信息
        double bandwidth_kbps;        // 带宽 (kb/s)
        double video_fps;             // 视频帧率
        double audio_packets_per_sec; // 音频包发送速率
        int64_t total_bytes_sent;     // 总发送字节数
    };
    Stats get_stats() const;
    
    // 重置统计信息
    void reset_stats();
    
private:
    // 推流线程函数
    void push_thread_func();
    
    // 尝试重新连接
    ErrorCode try_reconnect();
    
    // 设置推流状态
    void set_state(StreamState state);
    
    // 推流配置
    StreamConfig config_;
    
    // 推流状态
    std::atomic<StreamState> state_;
    
    // 推流队列
    PushQueue push_queue_;
    
    // RTMP推流器
    RTMPPusher rtmp_pusher_;
    
    // 推流线程
    std::thread push_thread_;
    
    // 推流线程停止标志
    std::atomic<bool> stop_thread_;
    
    // 重新连接尝试次数
    std::atomic<int> reconnect_attempts_;
};

} // namespace live_assistant
