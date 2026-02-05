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

    // 输入格式为 S16（与原项目一致）
    AVChannelLayout in_layout = make_channel_layout(config_.channels);
    AVChannelLayout out_layout = codec_ctx_->ch_layout;

    if (swr_alloc_set_opts2(
            &swr_,
            &out_layout,
            AV_SAMPLE_FMT_FLTP,  // AAC需要 FLTP 格式
            codec_ctx_->sample_rate,
            &in_layout,
            AV_SAMPLE_FMT_S16,   // 输入为 S16 格式
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
    last_audio_timestamp_ = -1;

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
    codec_ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;  // AAC编码必须使用 FLTP

    codec_ctx_->profile = FF_PROFILE_AAC_LOW;

    // For FLV/RTMP, extradata (AudioSpecificConfig) is required
    codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // 设置AAC编码参数（与原项目一致）
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "aac_coder", "twoloop", 0);
    av_dict_set(&opts, "prediction", "1", 0);
    av_dict_set(&opts, "aac_pred", "1", 0);
    av_dict_set(&opts, "cutoff", "18000", 0);

    if (avcodec_open2(codec_ctx_, codec_, &opts) < 0) {
        LOG_ERROR("Failed to open AAC encoder");
        av_dict_free(&opts);
        return ErrorCode::INIT_FAILED;
    }
    av_dict_free(&opts);

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

    packet_ = av_packet_alloc();
    if (!packet_) {
        LOG_ERROR("Failed to alloc audio packet");
        return ErrorCode::INIT_FAILED;
    }

    ErrorCode swr_ret = ensure_swr();
    if (swr_ret != ErrorCode::SUCCESS) {
        return swr_ret;
    }

    // Log detailed codec configuration
    LOG_INFO("[AACEncoder] Initialized successfully:");
    LOG_INFO("  - sample_rate: " + std::to_string(config_.sample_rate));
    LOG_INFO("  - channels: " + std::to_string(config_.channels));
    LOG_INFO("  - bitrate: " + std::to_string(config_.bitrate));
    LOG_INFO("  - codec frame_size: " + std::to_string(codec_ctx_->frame_size));
    LOG_INFO("  - sample_fmt: " + std::to_string(codec_ctx_->sample_fmt));
    LOG_INFO("  - extradata_size: " + std::to_string(codec_ctx_->extradata_size));
    LOG_INFO("  - time_base: " + std::to_string(codec_ctx_->time_base.num) + "/" + std::to_string(codec_ctx_->time_base.den));

    initialized_ = true;
    return ErrorCode::SUCCESS;
}

ErrorCode AACEncoder::shutdown() {
    initialized_ = false;

    if (packet_) {
        av_packet_free(&packet_);
        packet_ = nullptr;
    }

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
    last_audio_timestamp_ = -1;
    input_buffer_.clear();

    return ErrorCode::SUCCESS;
}

// 新增：接收原始音频数据（与原项目的 encodeData 对应）
void AACEncoder::encode_audio_data(const QByteArray& data, int64_t timestamp) {
    if (!initialized_ || data.isEmpty()) {
        return;
    }

    static int encode_count = 0;
    encode_count++;

    // 降低日志输出频率
    if (encode_count % 1000 == 0) {
        LOG_INFO("[AACEncoder] Audio encoding: count=" + std::to_string(encode_count) +
                 ", buffer size=" + std::to_string(input_buffer_.size()));
    }

    // 添加数据到输入缓冲区
    input_buffer_.append(data.constData(), data.size());

    // 确保使用正确的输入格式（S16格式）
    int input_bytes_per_sample = sizeof(int16_t);
    int bytes_per_frame = codec_ctx_->frame_size * input_bytes_per_sample * config_.channels;

    if (bytes_per_frame <= 0) {
        if (encode_count % 500 == 0) {
            LOG_WARNING("[AACEncoder] Invalid bytesPerFrame: " + std::to_string(bytes_per_frame));
        }
        return;
    }

    // 限制一次处理的帧数，避免长时间占用CPU
    int max_frames_per_call = 2;
    int frames_processed = 0;

    while (input_buffer_.size() >= bytes_per_frame && frames_processed < max_frames_per_call) {
        QByteArray frame_data = input_buffer_.left(bytes_per_frame);
        input_buffer_.remove(0, bytes_per_frame);

        // 转换输入数据到编码器所需格式
        int frame_samples = convert_input_data(
            reinterpret_cast<const uint8_t*>(frame_data.constData()),
            frame_data.size(),
            frame_
        );

        if (frame_samples <= 0) {
            if (encode_count % 500 == 0) {
                LOG_WARNING("[AACEncoder] Invalid frame samples: " + std::to_string(frame_samples));
            }
            continue;
        }

        // 使用内部PTS计数器，确保每次递增 frame_size (1024)
        // AAC编码器要求固定的帧大小，PTS必须按frame_size递增
        frame_->pts = next_pts_;
        next_pts_ += codec_ctx_->frame_size;  // 递增1024采样

        // 发送帧到编码器
        int ret = avcodec_send_frame(codec_ctx_, frame_);
        if (ret < 0) {
            if (encode_count % 500 == 0) {
                char errbuf[1024];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_WARNING("[AACEncoder] avcodec_send_frame failed: " + std::string(errbuf));
            }
            continue;
        }

        // 接收编码后的数据包
        while (ret >= 0) {
            ret = avcodec_receive_packet(codec_ctx_, packet_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                if (encode_count % 500 == 0) {
                    char errbuf[1024];
                    av_strerror(ret, errbuf, sizeof(errbuf));
                    LOG_WARNING("[AACEncoder] avcodec_receive_packet failed: " + std::string(errbuf));
                }
                break;
            }

            // 发送编码完成信号（过滤6字节异常帧，与原项目一致）
            if (packet_->size > 0) {
                // if (packet_->size == 6) {
                //     // 过滤掉已知的6字节异常帧
                //     LOG_WARNING("[AACEncoder] Filtered abnormal 6-byte AAC frame");
                // } else
                {
                    // 发送所有其他大小的音频帧，包括静音帧
                    // 使用 packet_->pts（编码器输出的PTS）而不是外部传入的timestamp
                    emit audio_encoded(packet_->data, packet_->size, packet_->pts);
                    frames_processed++;
                }
            }
            av_packet_unref(packet_);
        }
    }
}

// 新增：处理原始音频数据（内部实现）
void AACEncoder::process_audio_data(const QByteArray& data, int64_t timestamp) {
    encode_audio_data(data, timestamp);
}

// 新增：转换输入数据（与原项目的 convertInputData 对应）
int AACEncoder::convert_input_data(const uint8_t* input_data, int input_size, AVFrame* frame) {
    if (!input_data || input_size <= 0 || !frame) return 0;

    if (!swr_) {
        LOG_ERROR("[AACEncoder] Resampler not initialized");
        return 0;
    }

    int bytes_per_sample = sizeof(int16_t);
    int input_samples = input_size / (bytes_per_sample * config_.channels);

    if (input_samples <= 0) {
        LOG_WARNING("[AACEncoder] Invalid input sample count");
        return 0;
    }

    // 先清零frame数据，避免垃圾数据
    for (int ch = 0; ch < frame->ch_layout.nb_channels; ch++) {
        if (frame->data[ch]) {
            memset(frame->data[ch], 0, frame->nb_samples * sizeof(float));
        }
    }

    const uint8_t* input_data_arr[1] = {input_data};
    int ret = swr_convert(
        swr_,
        frame->data,
        frame->nb_samples,
        input_data_arr,
        input_samples
    );

    if (ret < 0) {
        LOG_WARNING("[AACEncoder] swr_convert failed, ret=" + std::to_string(ret));
        return 0;
    }

    // 如果转换的样本数少于frame容量，剩余部分已经是静音（已清零）
    // 返回实际转换的样本数，但frame已经包含完整的frame->nb_samples数据（部分为静音）
    return frame->nb_samples;  // 返回完整帧大小，确保AAC编码器收到固定大小的帧
}

ErrorCode AACEncoder::send_frame_internal(const std::shared_ptr<AudioFrame>& in) {
    // 保留此方法以兼容旧接口，内部转换为新的处理方式
    if (!in || !in->raw_data) {
        return ErrorCode::INVALID_PARAM;
    }

    // 将 AudioFrame 转换为 QByteArray
    int data_size = in->samples * in->channels * sizeof(int16_t);
    QByteArray data(reinterpret_cast<const char*>(in->raw_data), data_size);

    // 使用新的处理方式
    encode_audio_data(data, in->timestamp_ms);

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

        // Skip invalid/empty packets
        if (!pkt->data || pkt->size <= 0) {
            LOG_WARNING("[AACEncoder] Received empty packet from encoder, skipping");
            av_packet_free(&pkt);
            continue;
        }

        // Ensure packet is reference-counted
        if (av_packet_make_refcounted(pkt) < 0) {
            LOG_WARNING("[AACEncoder] av_packet_make_refcounted failed");
        }

        auto out = std::make_shared<EncodedPacket>();
        out->type = MediaType::AUDIO;
        out->pts = pkt->pts;
        out->dts = pkt->dts;

        if (pkt->duration <= 0 && codec_ctx_->frame_size > 0) {
            out->duration = codec_ctx_->frame_size;
        } else {
            out->duration = pkt->duration;
        }

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

    // 使用新的处理方式
    ErrorCode sret = send_frame_internal(frame);
    if (sret != ErrorCode::SUCCESS) {
        LOG_ERROR("[AACEncoder] send_frame_internal failed");
        return sret;
    }

    // 旧接口需要返回 packets，所以继续使用 receive_packets
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

ErrorCode AACEncoder::reset() {
    if (!initialized_) {
        return ErrorCode::INIT_FAILED;
    }

    // 清空输入缓冲区
    input_buffer_.clear();

    // 刷新编码器缓冲区
    if (codec_ctx_) {
        avcodec_flush_buffers(codec_ctx_);
        LOG_DEBUG("[AACEncoder] Flushed codec buffers");
    }

    // 排空所有残留的数据包
    int total_flushed = 0;
    while (true) {
        std::vector<EncodedPacketPtr> flushed_packets;
        ErrorCode flush_result = receive_packets(flushed_packets);
        if (flush_result != ErrorCode::SUCCESS || flushed_packets.empty()) {
            break;
        }
        total_flushed += static_cast<int>(flushed_packets.size());
    }

    if (total_flushed > 0) {
        LOG_INFO("[AACEncoder] Flushed " + std::to_string(total_flushed) + " residual packets");
    }

    // 重置时间戳计数器
    next_pts_ = 0;
    last_audio_timestamp_ = -1;

    LOG_INFO("[AACEncoder] Reset complete");
    return ErrorCode::SUCCESS;
}

// 保留此函数以兼容旧接口（虽然新实现不再使用它）
void AACEncoder::write_samples_to_frame(const std::vector<std::vector<float>>& pending,
                                         int samples_to_write,
                                         AVFrame* frame,
                                         int fmt,
                                         int channels) {
    // 新实现不再使用此函数，保留以避免编译错误
    AVSampleFormat sample_fmt = static_cast<AVSampleFormat>(fmt);
    if (sample_fmt == AV_SAMPLE_FMT_FLTP) {
        for (int ch = 0; ch < channels; ++ch) {
            float* dst = reinterpret_cast<float*>(frame->data[ch]);
            const auto& vec = pending[ch];
            for (int i = 0; i < samples_to_write && i < static_cast<int>(vec.size()); ++i) {
                dst[i] = vec[i];
            }
        }
    }
}

} // namespace live_assistant
