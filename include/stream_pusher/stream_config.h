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
    // 30fps视频约1秒的包量（30视频+约46音频=76），设为80可快速排空
    int max_queue_size = 80;
    
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

    // 验证配置是否有效
    bool is_valid() const {
        // 检查服务器URL是否为空
        if (server_url.empty()) {
            return false;
        }

        // 检查是否以rtmp://或rtmps://开头
        if (server_url.find("rtmp://") != 0 && server_url.find("rtmps://") != 0) {
            return false;
        }

        // 检查stream_key是否为空
        if (stream_key.empty()) {
            return false;
        }

        // 基本的URL格式检查：应该包含至少一个点（域名）
        std::string full_url = server_url + "/" + stream_key;
        if (full_url.find('.') == std::string::npos) {
            return false;
        }

        return true;
    }

    // 获取完整的推流URL
    std::string get_full_url() const {
        return server_url + "/" + stream_key;
    }
};

} // namespace live_assistant