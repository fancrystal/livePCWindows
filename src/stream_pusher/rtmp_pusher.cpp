#include "stream_pusher/rtmp_pusher.h"
#include "common/log.h"
#include "common/error.h"

// FFmpeg includes
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

namespace live_assistant {

RTMPPusher::RTMPPusher() {
    // Register FFmpeg network protocols
    avformat_network_init();
    LOG_INFO("RTMPPusher constructor");
}

RTMPPusher::~RTMPPusher() {
    disconnect();
    free_resources();
    avformat_network_deinit();
    LOG_INFO("RTMPPusher destructor");
}

ErrorCode RTMPPusher::initialize(const StreamConfig& config) {
    LOG_INFO("Initializing RTMP pusher");
    
    // Store configuration
    config_ = config;
    
    // Initialize format context
    ErrorCode result = init_format_context();
    if (result != ErrorCode::SUCCESS) {
        LOG_ERROR("Failed to initialize format context");
        return result;
    }
    
    // Add streams
    result = add_audio_stream();
    if (result != ErrorCode::SUCCESS) {
        LOG_ERROR("Failed to add audio stream");
        free_resources();
        return result;
    }
    
    result = add_video_stream();
    if (result != ErrorCode::SUCCESS) {
        LOG_ERROR("Failed to add video stream");
        free_resources();
        return result;
    }
    
    // Write header
    if (avformat_write_header(format_ctx_, nullptr) < 0) {
        LOG_ERROR("Failed to write header");
        free_resources();
        return ErrorCode::INIT_FAILED;
    }
    
    LOG_INFO("RTMP pusher initialized successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode RTMPPusher::init_format_context() {
    // Create format context
    if (avformat_alloc_output_context2(&format_ctx_, nullptr, "flv", nullptr) < 0) {
        LOG_ERROR("Failed to allocate output context");
        return ErrorCode::INIT_FAILED;
    }
    
    // Set low latency options if enabled
    if (config_.low_latency) {
        av_opt_set(format_ctx_->priv_data, "rtmp_live", "live", 0);
        av_opt_set(format_ctx_->priv_data, "rtmp_buffer", "0", 0);
        av_opt_set(format_ctx_->priv_data, "preset", "ultrafast", 0);
    }
    
    return ErrorCode::SUCCESS;
}

ErrorCode RTMPPusher::add_audio_stream() {
    // Add audio stream (will be configured when first audio packet arrives)
    audio_stream_ = avformat_new_stream(format_ctx_, nullptr);
    if (!audio_stream_) {
        LOG_ERROR("Failed to create audio stream");
        return ErrorCode::INIT_FAILED;
    }
    
    // Set stream index
    audio_stream_->index = format_ctx_->nb_streams - 1;
    audio_stream_->id = format_ctx_->nb_streams - 1;
    
    return ErrorCode::SUCCESS;
}

ErrorCode RTMPPusher::add_video_stream() {
    // Add video stream (will be configured when first video packet arrives)
    video_stream_ = avformat_new_stream(format_ctx_, nullptr);
    if (!video_stream_) {
        LOG_ERROR("Failed to create video stream");
        return ErrorCode::INIT_FAILED;
    }
    
    // Set stream index
    video_stream_->index = format_ctx_->nb_streams - 1;
    video_stream_->id = format_ctx_->nb_streams - 1;
    
    return ErrorCode::SUCCESS;
}

ErrorCode RTMPPusher::connect() {
    LOG_INFO("Connecting to RTMP server: " + config_.server_url);
    
    // Create full stream URL
    std::string full_url = config_.server_url + "/" + config_.stream_key;
    
    // Open output URL
    if (avio_open(&format_ctx_->pb, full_url.c_str(), AVIO_FLAG_WRITE) < 0) {
        LOG_ERROR("Failed to open output URL: " + full_url);
        return ErrorCode::CONNECT_FAILED;
    }
    
    connected_ = true;
    stats_.connected = true;
    stats_.reconnect_attempts++;
    
    LOG_INFO("Connected to RTMP server successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode RTMPPusher::disconnect() {
    if (!connected_) {
        return ErrorCode::SUCCESS;
    }
    
    LOG_INFO("Disconnecting from RTMP server");
    
    // Write trailer
    if (format_ctx_) {
        av_write_trailer(format_ctx_);
    }
    
    // Close output URL
    if (format_ctx_ && format_ctx_->pb) {
        avio_close(format_ctx_->pb);
        format_ctx_->pb = nullptr;
    }
    
    connected_ = false;
    stats_.connected = false;
    
    LOG_INFO("Disconnected from RTMP server");
    return ErrorCode::SUCCESS;
}

ErrorCode RTMPPusher::send_packet(const MediaPacket& packet) {
    if (!connected_) {
        return ErrorCode::NOT_CONNECTED;
    }
    
    // Check format context
    if (!format_ctx_ || !format_ctx_->pb) {
        return ErrorCode::NOT_CONNECTED;
    }
    
    ErrorCode result = ErrorCode::SUCCESS;
    
    // Handle different packet types
    if (packet.is_config) {
        result = send_config_packet(packet);
    } else if (packet.type == MediaType::AUDIO) {
        result = send_audio_packet(packet);
    } else if (packet.type == MediaType::VIDEO) {
        result = send_video_packet(packet);
    }
    
    return result;
}

ErrorCode RTMPPusher::send_config_packet(const MediaPacket& packet) {
    LOG_DEBUG("Sending config packet");
    
    // 根据包类型发送不同的配置包
    if (packet.type == MediaType::AUDIO) {
        // 处理音频配置包
        LOG_DEBUG("Sending audio config packet, size: " + std::to_string(packet.data.size()));
        return send_audio_packet(packet);
    } else if (packet.type == MediaType::VIDEO) {
        // 处理视频配置包
        LOG_DEBUG("Sending video config packet, size: " + std::to_string(packet.data.size()));
        return send_video_packet(packet);
    } else {
        LOG_ERROR("Unknown media type for config packet");
        return ErrorCode::INVALID_PARAM;
    }
}

ErrorCode RTMPPusher::send_audio_packet(const MediaPacket& packet) {
    // Create AV packet
    AVPacket av_pkt;
    av_init_packet(&av_pkt);
    av_pkt.data = const_cast<uint8_t*>(packet.data.data());
    av_pkt.size = packet.data.size();
    av_pkt.stream_index = audio_stream_->index;
    
    // Set timestamp
    av_pkt.pts = av_pkt.dts = packet.timestamp / 1000; // Convert to milliseconds
    
    // Send packet
    int ret = av_interleaved_write_frame(format_ctx_, &av_pkt);
    av_packet_unref(&av_pkt);
    
    if (ret < 0) {
        LOG_ERROR("Failed to send audio packet");
        return ErrorCode::SEND_FAILED;
    }
    
    // Update stats
    stats_.audio_packets_sent++;
    stats_.bytes_sent += packet.data.size();
    
    return ErrorCode::SUCCESS;
}

ErrorCode RTMPPusher::send_video_packet(const MediaPacket& packet) {
    // Create AV packet
    AVPacket av_pkt;
    av_init_packet(&av_pkt);
    av_pkt.data = const_cast<uint8_t*>(packet.data.data());
    av_pkt.size = packet.data.size();
    av_pkt.stream_index = video_stream_->index;
    
    // Set keyframe flag
    if (packet.is_keyframe) {
        av_pkt.flags |= AV_PKT_FLAG_KEY;
    }
    
    // Set timestamp
    av_pkt.pts = av_pkt.dts = packet.timestamp / 1000; // Convert to milliseconds
    
    // Send packet
    int ret = av_interleaved_write_frame(format_ctx_, &av_pkt);
    av_packet_unref(&av_pkt);
    
    if (ret < 0) {
        LOG_ERROR("Failed to send video packet");
        return ErrorCode::SEND_FAILED;
    }
    
    // Update stats
    stats_.video_packets_sent++;
    stats_.bytes_sent += packet.data.size();
    
    return ErrorCode::SUCCESS;
}

bool RTMPPusher::is_connected() const {
    return connected_;
}

RTMPPusher::Stats RTMPPusher::get_stats() const {
    return stats_;
}

void RTMPPusher::reset_stats() {
    stats_ = {connected_, 0, 0, 0, 0};
}

void RTMPPusher::free_resources() {
    if (format_ctx_) {
        // Free streams (handled by avformat_free_context)
        audio_stream_ = nullptr;
        video_stream_ = nullptr;
        
        // Free format context
        avformat_free_context(format_ctx_);
        format_ctx_ = nullptr;
    }
    
    connected_ = false;
    stats_.connected = false;
}

} // namespace live_assistant