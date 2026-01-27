#include "encoder/audio_encoder.h"
#include "common/log.h"
#include "common/error.h"
#include "audio_engine/audio_engine.h"  // For AudioFrame definition

#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
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

    // Prefer FLTP (planar float) for higher fidelity and to avoid quantization artifacts
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
            LOG_WARNING("Requested AV_SAMPLE_FMT_FLTP not supported by codec; falling back to first supported format");
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

    // Initialize pending planar buffers based on configured channels
    pending_planar_samples_.assign(config_.channels, std::vector<float>());
    pending_samples_per_channel_ = 0;

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

    // Diagnostic: print first few input samples to help debugging (limited)
    if (in->raw_data) {
        int dbgN = std::min(8, in->samples * in->channels);
        std::string s;
        for (int i = 0; i < dbgN; ++i) {
            s += std::to_string(in->raw_data[i]);
            if (i + 1 < dbgN) s += ",";
        }
        LOG_DEBUG(std::string("AACEncoder::send_frame_internal input samples[0..") + std::to_string(dbgN-1) + "]=" + s);
    }

    int converted = swr_convert(
        swr_,
        frame_->data,
        frame_capacity,
        in_data,
        in->samples);

    if (converted < 0) {
        char errbuf[128] = {0};
        av_strerror(converted, errbuf, sizeof(errbuf));
        LOG_ERROR(std::string("swr_convert failed: ") + errbuf);
        return ErrorCode::ENCODING_ERROR;
    }

    // Append converted planar float samples into pending buffer
    int channels = config_.channels;
    int out_samples = converted; // per-channel samples
    // Ensure pending structure initialized
    if (pending_planar_samples_.size() != static_cast<size_t>(channels)) {
        pending_planar_samples_.assign(channels, std::vector<float>());
        pending_samples_per_channel_ = 0;
    }

    AVSampleFormat out_fmt = codec_ctx_->sample_fmt;
    if (out_fmt == AV_SAMPLE_FMT_FLTP) {
        for (int ch = 0; ch < channels; ++ch) {
            float* out_ptr = reinterpret_cast<float*>(frame_->data[ch]);
            auto &vec = pending_planar_samples_[ch];
            vec.insert(vec.end(), out_ptr, out_ptr + out_samples);
        }
    } else if (out_fmt == AV_SAMPLE_FMT_S16P) {
        for (int ch = 0; ch < channels; ++ch) {
            int16_t* out_ptr = reinterpret_cast<int16_t*>(frame_->data[ch]);
            auto &vec = pending_planar_samples_[ch];
            for (int i = 0; i < out_samples; ++i) {
                vec.push_back(static_cast<float>(out_ptr[i]) / 32768.0f);
            }
        }
    } else if (out_fmt == AV_SAMPLE_FMT_S16) {
        // packed interleaved int16 in frame_->data[0]
        int16_t* packed = reinterpret_cast<int16_t*>(frame_->data[0]);
        for (int i = 0; i < out_samples; ++i) {
            for (int ch = 0; ch < channels; ++ch) {
                auto &vec = pending_planar_samples_[ch];
                int16_t v = packed[i * channels + ch];
                vec.push_back(static_cast<float>(v) / 32768.0f);
            }
        }
    } else {
        // fallback: try to interpret as float planar
        LOG_WARNING("AACEncoder::send_frame_internal unexpected out_fmt, assuming FLTP");
        for (int ch = 0; ch < channels; ++ch) {
            float* out_ptr = reinterpret_cast<float*>(frame_->data[ch]);
            auto &vec = pending_planar_samples_[ch];
            vec.insert(vec.end(), out_ptr, out_ptr + out_samples);
        }
    }
    pending_samples_per_channel_ += out_samples;

    int required = codec_ctx_->frame_size > 0 ? codec_ctx_->frame_size : 0;
    if (required <= 0) {
        // If codec doesn't require fixed frame_size, send what we have immediately.
        frame_->nb_samples = out_samples;
        frame_->pts = next_pts_;
        next_pts_ += out_samples;
        int send_ret = avcodec_send_frame(codec_ctx_, frame_);
        if (send_ret < 0) {
            char errbuf2[128] = {0};
            av_strerror(send_ret, errbuf2, sizeof(errbuf2));
            LOG_ERROR(std::string("avcodec_send_frame failed: ") + errbuf2 + " (ret=" + std::to_string(send_ret) + ")");
            return ErrorCode::ENCODING_ERROR;
        }
        return ErrorCode::SUCCESS;
    }

    // While we have at least one full frame worth of samples, create and send frames
    while (pending_samples_per_channel_ >= required) {
        // prepare frame buffer
        if (av_frame_make_writable(frame_) < 0) {
            return ErrorCode::INIT_FAILED;
        }

        // Write samples into frame_->data according to codec sample_fmt
        AVSampleFormat out_fmt = codec_ctx_->sample_fmt;

        if (out_fmt == AV_SAMPLE_FMT_FLTP) {
            // planar float
            for (int ch = 0; ch < channels; ++ch) {
                float* dst = reinterpret_cast<float*>(frame_->data[ch]);
                auto &vec = pending_planar_samples_[ch];
                for (int i = 0; i < required; ++i) {
                    dst[i] = vec[i];
                }
                vec.erase(vec.begin(), vec.begin() + required);
            }
        } else if (out_fmt == AV_SAMPLE_FMT_S16P) {
            // planar int16
            for (int ch = 0; ch < channels; ++ch) {
                int16_t* dst = reinterpret_cast<int16_t*>(frame_->data[ch]);
                auto &vec = pending_planar_samples_[ch];
                for (int i = 0; i < required; ++i) {
                    float v = vec[i];
                    int32_t iv = static_cast<int32_t>(std::round(v * 32767.0f));
                    if (iv > 32767) iv = 32767;
                    if (iv < -32768) iv = -32768;
                    dst[i] = static_cast<int16_t>(iv);
                }
                vec.erase(vec.begin(), vec.begin() + required);
            }
        } else if (out_fmt == AV_SAMPLE_FMT_S16) {
            // packed int16 interleaved in frame_->data[0]
            int16_t* dst = reinterpret_cast<int16_t*>(frame_->data[0]);
            for (int i = 0; i < required; ++i) {
                for (int ch = 0; ch < channels; ++ch) {
                    auto &vec = pending_planar_samples_[ch];
                    float v = vec[i];
                    int32_t iv = static_cast<int32_t>(std::round(v * 32767.0f));
                    if (iv > 32767) iv = 32767;
                    if (iv < -32768) iv = -32768;
                    dst[i * channels + ch] = static_cast<int16_t>(iv);
                }
            }
            for (int ch = 0; ch < channels; ++ch) {
                auto &vec = pending_planar_samples_[ch];
                vec.erase(vec.begin(), vec.begin() + required);
            }
        } else {
            // fallback: try planar float write
            LOG_WARNING("Unsupported codec sample_fmt in write-back, attempting FLTP write");
            for (int ch = 0; ch < channels; ++ch) {
                float* dst = reinterpret_cast<float*>(frame_->data[ch]);
                auto &vec = pending_planar_samples_[ch];
                for (int i = 0; i < required; ++i) dst[i] = vec[i];
                vec.erase(vec.begin(), vec.begin() + required);
            }
        }

        // update counters and pts
        frame_->nb_samples = required;
        frame_->pts = next_pts_;
        next_pts_ += required;
        pending_samples_per_channel_ -= required;

        int send_ret = avcodec_send_frame(codec_ctx_, frame_);
        if (send_ret < 0) {
            char errbuf2[128] = {0};
            av_strerror(send_ret, errbuf2, sizeof(errbuf2));
            LOG_ERROR(std::string("avcodec_send_frame failed: ") + errbuf2 + " (ret=" + std::to_string(send_ret) + ")");
            return ErrorCode::ENCODING_ERROR;
        }
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
