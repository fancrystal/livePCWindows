#include "stream_pusher/rtmp_pusher.h"
#include "common/log.h"
#include "common/error.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

namespace live_assistant {

RTMPPusher::RTMPPusher() {
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

    config_ = config;
    header_written_ = false;

    ErrorCode result = init_format_context();
    if (result != ErrorCode::SUCCESS) {
        LOG_ERROR("Failed to initialize format context");
        return result;
    }

    LOG_INFO("RTMP pusher initialized successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode RTMPPusher::init_format_context() {
    free_resources();

    if (avformat_alloc_output_context2(&format_ctx_, nullptr, "flv", nullptr) < 0) {
        LOG_ERROR("Failed to allocate output context");
        return ErrorCode::INIT_FAILED;
    }

    if (config_.low_latency && format_ctx_->priv_data) {
        av_opt_set(format_ctx_->priv_data, "rtmp_live", "live", 0);
        av_opt_set(format_ctx_->priv_data, "rtmp_buffer", "0", 0);
    }

    return ErrorCode::SUCCESS;
}

ErrorCode RTMPPusher::register_audio_stream(AVCodecParameters* codecpar, AVRational time_base) {
    if (!format_ctx_) {
        return ErrorCode::INIT_FAILED;
    }
    if (header_written_) {
        LOG_ERROR("Cannot register stream after header written");
        return ErrorCode::INVALID_STATE;
    }

    audio_stream_ = avformat_new_stream(format_ctx_, nullptr);
    if (!audio_stream_) {
        LOG_ERROR("Failed to create audio stream");
        return ErrorCode::INIT_FAILED;
    }

    if (avcodec_parameters_copy(audio_stream_->codecpar, codecpar) < 0) {
        LOG_ERROR("Failed to copy audio codec parameters");
        return ErrorCode::INIT_FAILED;
    }

    audio_stream_->time_base = time_base;
    return ErrorCode::SUCCESS;
}

ErrorCode RTMPPusher::register_video_stream(AVCodecParameters* codecpar, AVRational time_base) {
    if (!format_ctx_) {
        return ErrorCode::INIT_FAILED;
    }
    if (header_written_) {
        LOG_ERROR("Cannot register stream after header written");
        return ErrorCode::INVALID_STATE;
    }

    video_stream_ = avformat_new_stream(format_ctx_, nullptr);
    if (!video_stream_) {
        LOG_ERROR("Failed to create video stream");
        return ErrorCode::INIT_FAILED;
    }

    if (avcodec_parameters_copy(video_stream_->codecpar, codecpar) < 0) {
        LOG_ERROR("Failed to copy video codec parameters");
        return ErrorCode::INIT_FAILED;
    }

    video_stream_->time_base = time_base;
    return ErrorCode::SUCCESS;
}

ErrorCode RTMPPusher::open_output() {
    if (!format_ctx_) {
        return ErrorCode::INIT_FAILED;
    }

    std::string full_url = config_.server_url + "/" + config_.stream_key;

    if (avio_open(&format_ctx_->pb, full_url.c_str(), AVIO_FLAG_WRITE) < 0) {
        LOG_ERROR("Failed to open output URL: " + full_url);
        return ErrorCode::CONNECT_FAILED;
    }

    connected_ = true;
    stats_.connected = true;
    stats_.reconnect_attempts++;

    return ErrorCode::SUCCESS;
}

ErrorCode RTMPPusher::connect_and_write_header() {
    if (connected_ && header_written_) {
        return ErrorCode::SUCCESS;
    }

    if (!audio_stream_ || !video_stream_) {
        LOG_ERROR("Audio/video streams not registered before connect_and_write_header");
        return ErrorCode::INVALID_STATE;
    }

    ErrorCode result = open_output();
    if (result != ErrorCode::SUCCESS) {
        return result;
    }

    if (avformat_write_header(format_ctx_, nullptr) < 0) {
        LOG_ERROR("Failed to write header");
        disconnect();
        return ErrorCode::INIT_FAILED;
    }

    header_written_ = true;
    LOG_INFO("Connected and wrote RTMP header");
    return ErrorCode::SUCCESS;
}

ErrorCode RTMPPusher::disconnect() {
    if (!connected_) {
        return ErrorCode::SUCCESS;
    }

    LOG_INFO("Disconnecting from RTMP server");

    if (format_ctx_ && header_written_) {
        av_write_trailer(format_ctx_);
    }

    if (format_ctx_ && format_ctx_->pb) {
        avio_close(format_ctx_->pb);
        format_ctx_->pb = nullptr;
    }

    connected_ = false;
    header_written_ = false;
    stats_.connected = false;

    LOG_INFO("Disconnected from RTMP server");
    return ErrorCode::SUCCESS;
}

ErrorCode RTMPPusher::send_packet(const EncodedPacketPtr& packet) {
    if (!packet) {
        return ErrorCode::INVALID_PARAM;
    }

    if (!connected_ || !header_written_ || !format_ctx_ || !format_ctx_->pb) {
        return ErrorCode::NOT_CONNECTED;
    }

    if (!packet->pkt) {
        return ErrorCode::INVALID_PARAM;
    }

    AVPacket* avpkt = packet->pkt.get();

    // Determine target stream and its time_base
    AVStream* st = nullptr;
    if (packet->type == MediaType::AUDIO) {
        if (!audio_stream_) {
            return ErrorCode::INVALID_STATE;
        }
        st = audio_stream_;
        stats_.audio_packets_sent++;
    } else {
        if (!video_stream_) {
            return ErrorCode::INVALID_STATE;
        }
        st = video_stream_;
        stats_.video_packets_sent++;

        if (packet->is_keyframe) {
            avpkt->flags |= AV_PKT_FLAG_KEY;
        }
    }

    avpkt->stream_index = st->index;

    // Set packet timestamps in encoder time_base then rescale into stream time_base
    avpkt->pts = packet->pts;
    avpkt->dts = packet->dts;
    avpkt->duration = packet->duration;

    if (packet->encoder_time_base.num > 0 && packet->encoder_time_base.den > 0) {
        av_packet_rescale_ts(avpkt, packet->encoder_time_base, st->time_base);
    }

    int ret = av_interleaved_write_frame(format_ctx_, avpkt);
    if (ret < 0) {
        LOG_ERROR("Failed to send packet");
        return ErrorCode::SEND_FAILED;
    }

    stats_.bytes_sent += avpkt->size;
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
        audio_stream_ = nullptr;
        video_stream_ = nullptr;
        avformat_free_context(format_ctx_);
        format_ctx_ = nullptr;
    }

    connected_ = false;
    header_written_ = false;
    stats_.connected = false;
}

} // namespace live_assistant
