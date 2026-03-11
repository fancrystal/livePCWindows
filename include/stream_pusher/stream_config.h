#pragma once

#include <string>

namespace live_assistant {

// 推流协议枚举
enum class StreamProtocol {
    RTMP,
    RTMPS
};

// 推流配置结构体
struct StreamConfig {
    // 服务器URL (rtmp://server/live 或 rtmps://server/live)
    std::string server_url;
    
    // 推流密钥
    std::string stream_key;
    
    // 协议类型
    StreamProtocol protocol = StreamProtocol::RTMP;
    
    // 发送缓冲区大小（毫秒）
    int send_buffer_ms = 500;
    
    // 最大队列大小（包数量）
    int max_queue_size = 100;
    
    // 启用自动重连
    bool auto_reconnect = true;
    
    // 最大重连尝试次数
    int max_reconnect_attempts = 5;
    
    // 重连间隔（秒）
    int reconnect_interval_sec = 3;
    
    // 低延迟模式
    bool low_latency = true;
    
    // 使用交错写入模式 (av_interleaved_write_frame)
    // true = av_interleaved_write_frame (默认，更稳定，有延迟)
    // false = av_write_frame (更低延迟，可能不稳定)
    bool use_interleaved_write = true;
};

} // namespace live_assistant