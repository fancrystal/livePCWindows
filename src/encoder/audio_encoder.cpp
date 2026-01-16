#include "encoder/audio_encoder.h"
#include "common/log.h"
#include "common/error.h"
#include "audio_engine/audio_engine.h"  // For AudioFrame definition

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace live_assistant {

// ---- OpusEncoder (stub) ----
OpusEncoder::OpusEncoder() {
    LOG_INFO("OpusEncoder constructor");
}

OpusEncoder::~OpusEncoder() {
    shutdown();
    LOG_INFO("OpusEncoder destructor");
}

ErrorCode OpusEncoder::initialize(const AudioEncoderConfig& config) {
    config_ = config;
    initialized_ = true;

    time_base_ = AVRational{1, config_.sample_rate > 0 ? config_.sample_rate : 48000};
    if (!codecpar_) {
        codecpar_ = avcodec_parameters_alloc();
    }

    if (!codecpar_) {
        return ErrorCode::INIT_FAILED;
    }

    codecpar_->codec_type = AVMEDIA_TYPE_AUDIO;
    codecpar_->codec_id = AV_CODEC_ID_OPUS;
    codecpar_->sample_rate = config_.sample_rate;
    codecpar_->ch_layout.nb_channels = config_.channels;

    LOG_INFO("Opus encoder initialized (stub)");
    return ErrorCode::SUCCESS;
}

ErrorCode OpusEncoder::shutdown() {
    if (codecpar_) {
        avcodec_parameters_free(&codecpar_);
        codecpar_ = nullptr;
    }

    initialized_ = false;
    return ErrorCode::SUCCESS;
}

ErrorCode OpusEncoder::encode(const std::shared_ptr<AudioFrame>& /*frame*/, std::vector<EncodedPacketPtr>& packets) {
    packets.clear();
    return initialized_ ? ErrorCode::SUCCESS : ErrorCode::INIT_FAILED;
}

AVCodecParameters* OpusEncoder::get_codec_parameters() const {
    return codecpar_;
}

AVRational OpusEncoder::get_time_base() const {
    return time_base_;
}

ErrorCode OpusEncoder::set_bitrate(int bitrate) {
    if (!initialized_) {
        return ErrorCode::INIT_FAILED;
    }
    config_.bitrate = bitrate;
    return ErrorCode::SUCCESS;
}

// ---- AACEncoder ----
AACEncoder::AACEncoder() {
    LOG_INFO("AACEncoder constructor");
}

AACEncoder::~AACEncoder() {
    shutdown();
    LOG_INFO("AACEncoder destructor");
}

static AVChannelLayout make_channel_layout(int channels) {
#if LIBAVUTIL_VERSION_MAJOR >= 57
    AVChannelLayout layout;
    av_channel_layout_default(&layout, channels);
    return layout;
#else
    AVChannelLayout layout{};
    layout.nb_channels = channels;
    return layout;
#endif
}

AVCodecParameters* AACEncoder::get_codec_parameters() const {
    if (!codec_ctx_) {
        return nullptr;
    }

    AVCodecParameters* par = avcodec_parameters_alloc();
    if (!par) {
        return nullptr;
    }

    if (avcodec_parameters_from_context(par, codec_ctx_) < 0) {
        avcodec_parameters_free(&par);
        return nullptr;
    }

    return par;
}

AVRational AACEncoder::get_time_base() const {
    return AVRational{1, config_.sample_rate};
}

ErrorCode AACEncoder::ensure_swr() {
    if (swr_) {
        return ErrorCode::SUCCESS;
    }

    // Use channel layout API compatible with libavutil >=57
    AVChannelLayout in_layout = make_channel_layout(config_.channels);
    AVChannelLayout out_layout = codec_ctx_->ch_layout;

    if (swr_alloc_set_opts2(
            &swr_,
            &out_layout,
            codec_ctx_->sample_fmt,
            codec_ctx_->sample_rate,
            &in_layout,
            AV_SAMPLE_FMT_S16,
            config_.sample_rate,
            0,
            nullptr) < 0) {
        LOG_ERROR("Failed to alloc swr");
        return ErrorCode::INIT_FAILED;
    }

    if (!swr_ || swr_init(swr_) < 0) {
        if (swr_) {
            swr_free(&swr_);
        }
        LOG_ERROR("Failed to init swr");
        return ErrorCode::INIT_FAILED;
    }

    return ErrorCode::SUCCESS;
}

ErrorCode AACEncoder::initialize(const AudioEncoderConfig& config) {
    LOG_INFO("Initializing AAC encoder (FFmpeg)");

    config_ = config;
    next_pts_ = 0;

    codec_ = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec_) {
        LOG_ERROR("Failed to find AAC encoder");
        return ErrorCode::INIT_FAILED;
    }

    codec_ctx_ = avcodec_alloc_context3(codec_);
    if (!codec_ctx_) {
        LOG_ERROR("Failed to alloc AAC codec context");
        return ErrorCode::INIT_FAILED;
    }

    codec_ctx_->codec_type = AVMEDIA_TYPE_AUDIO;
    codec_ctx_->codec_id = AV_CODEC_ID_AAC;
    codec_ctx_->bit_rate = config_.bitrate;
    codec_ctx_->sample_rate = config_.sample_rate;
    codec_ctx_->time_base = AVRational{1, config_.sample_rate};

    codec_ctx_->ch_layout = make_channel_layout(config_.channels);

    // Prefer FLTP
    codec_ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    if (codec_->sample_fmts) {
        bool supported = false;
        for (const AVSampleFormat* f = codec_->sample_fmts; *f != AV_SAMPLE_FMT_NONE; ++f) {
            if (*f == codec_ctx_->sample_fmt) {
                supported = true;
                break;
            }
        }
        if (!supported) {
            codec_ctx_->sample_fmt = codec_->sample_fmts[0];
        }
    }

    codec_ctx_->profile = FF_PROFILE_AAC_LOW;

    if (avcodec_open2(codec_ctx_, codec_, nullptr) < 0) {
        LOG_ERROR("Failed to open AAC encoder");
        return ErrorCode::INIT_FAILED;
    }

    frame_ = av_frame_alloc();
    if (!frame_) {
        LOG_ERROR("Failed to alloc audio frame");
        return ErrorCode::INIT_FAILED;
    }

    frame_->format = codec_ctx_->sample_fmt;
    frame_->sample_rate = codec_ctx_->sample_rate;
    frame_->ch_layout = codec_ctx_->ch_layout;

    frame_->nb_samples = codec_ctx_->frame_size > 0 ? codec_ctx_->frame_size : 1024;

    if (av_frame_get_buffer(frame_, 0) < 0) {
        LOG_ERROR("Failed to alloc frame buffer");
        return ErrorCode::INIT_FAILED;
    }

    ErrorCode swr_ret = ensure_swr();
    if (swr_ret != ErrorCode::SUCCESS) {
        return swr_ret;
    }

    initialized_ = true;
    return ErrorCode::SUCCESS;
}

ErrorCode AACEncoder::shutdown() {
    initialized_ = false;

    if (swr_) {
        swr_free(&swr_);
        swr_ = nullptr;
    }

    if (frame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }

    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }

    codec_ = nullptr;
    next_pts_ = 0;

    return ErrorCode::SUCCESS;
}

ErrorCode AACEncoder::send_frame_internal(const std::shared_ptr<AudioFrame>& in) {
    if (!in || !codec_ctx_ || !frame_) {
        return ErrorCode::INVALID_PARAM;
    }

    if (in->channels != config_.channels) {
        LOG_ERROR("AACEncoder channel mismatch");
        return ErrorCode::INVALID_PARAM;
    }

    ErrorCode swr_ret = ensure_swr();
    if (swr_ret != ErrorCode::SUCCESS) {
        return swr_ret;
    }

    const uint8_t* in_data[1] = {reinterpret_cast<const uint8_t*>(in->raw_data)};

    const int frame_capacity = codec_ctx_->frame_size > 0 ? codec_ctx_->frame_size : frame_->nb_samples;
    frame_->nb_samples = frame_capacity;

    if (av_frame_make_writable(frame_) < 0) {
        return ErrorCode::INIT_FAILED;
    }

    int converted = swr_convert(
        swr_,
        frame_->data,
        frame_capacity,
        in_data,
        in->samples);

    if (converted < 0) {
        LOG_ERROR("swr_convert failed");
        return ErrorCode::ENCODING_ERROR;
    }

    frame_->nb_samples = converted;
    frame_->pts = next_pts_;
    next_pts_ += converted;

    if (avcodec_send_frame(codec_ctx_, frame_) < 0) {
        LOG_ERROR("avcodec_send_frame failed");
        return ErrorCode::ENCODING_ERROR;
    }

    return ErrorCode::SUCCESS;
}

ErrorCode AACEncoder::send_flush() {
    if (!codec_ctx_) {
        return ErrorCode::INIT_FAILED;
    }

    if (avcodec_send_frame(codec_ctx_, nullptr) < 0) {
        LOG_ERROR("avcodec_send_frame(flush) failed");
        return ErrorCode::ENCODING_ERROR;
    }

    return ErrorCode::SUCCESS;
}

ErrorCode AACEncoder::receive_packets(std::vector<EncodedPacketPtr>& packets) {
    packets.clear();

    if (!codec_ctx_) {
        return ErrorCode::INIT_FAILED;
    }

    while (true) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) {
            return ErrorCode::INIT_FAILED;
        }

        int ret = avcodec_receive_packet(codec_ctx_, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&pkt);
            break;
        }
        if (ret < 0) {
            av_packet_free(&pkt);
            LOG_ERROR("avcodec_receive_packet failed");
            return ErrorCode::ENCODING_ERROR;
        }

        auto out = std::make_shared<EncodedPacket>();
        out->type = MediaType::AUDIO;
        out->pts = pkt->pts;
        out->dts = pkt->dts;
        out->duration = pkt->duration;
        out->encoder_time_base = get_time_base();
        out->is_keyframe = false;
        out->priority = 2;
        out->pkt = AVPacketPtr(pkt);

        packets.push_back(std::move(out));
    }

    return ErrorCode::SUCCESS;
}

ErrorCode AACEncoder::encode(const std::shared_ptr<AudioFrame>& frame, std::vector<EncodedPacketPtr>& packets) {
    if (!initialized_) {
        LOG_ERROR("AAC encoder not initialized");
        return ErrorCode::INIT_FAILED;
    }

    packets.clear();

    ErrorCode sret = send_frame_internal(frame);
    if (sret != ErrorCode::SUCCESS) {
        return sret;
    }

    return receive_packets(packets);
}

ErrorCode AACEncoder::flush(std::vector<EncodedPacketPtr>& packets) {
    if (!initialized_) {
        return ErrorCode::INIT_FAILED;
    }

    packets.clear();

    ErrorCode sret = send_flush();
    if (sret != ErrorCode::SUCCESS) {
        return sret;
    }

    return receive_packets(packets);
}

ErrorCode AACEncoder::set_bitrate(int bitrate) {
    if (!initialized_) {
        return ErrorCode::INIT_FAILED;
    }

    config_.bitrate = bitrate;
    if (codec_ctx_) {
        codec_ctx_->bit_rate = bitrate;
    }

    return ErrorCode::SUCCESS;
}

} // namespace live_assistant
