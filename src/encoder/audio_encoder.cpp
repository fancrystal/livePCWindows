#include "encoder/audio_encoder.h"
#include "common/log.h"
#include "common/error.h"
#include "audio_engine/audio_engine.h"  // For AudioFrame definition

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
    // ✅ 返回与 codec_ctx_->time_base 一致的值
    // AAC 编码器使用采样数作为时间单位
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
    last_audio_timestamp_ = -1;
    input_buffer_.clear();

    return ErrorCode::SUCCESS;
}

// 接收原始音频数据并编码（使用 media_clock 时间戳确保音视频同步）
void AACEncoder::encode_audio_data(const QByteArray& data, int64_t timestamp) {
    if (!initialized_ || data.isEmpty()) {
        return;
    }

    static int64_t first_timestamp = -1;
    static int encode_count = 0;
    encode_count++;

    // 🔧 诊断第一帧问题
    if (first_timestamp == -1) {
        first_timestamp = timestamp;
        LOG_INFO("[AACEncoder] First audio frame timestamp: " + std::to_string(timestamp) + "ms");
    } else if (encode_count <= 5) {
        LOG_INFO("[AACEncoder] Frame " + std::to_string(encode_count) + 
                 ": timestamp=" + std::to_string(timestamp) + 
                 "ms, delta=" + std::to_string(timestamp - last_audio_timestamp_) + "ms");
    }

    // 🔧 检测时间戳回绕（异常跳跃）
    // 当推流重新开始时，media_clock 会从0开始，但 input_buffer_ 中可能还有旧数据
    if (last_audio_timestamp_ > 0 && timestamp < last_audio_timestamp_) {
        int64_t regression = last_audio_timestamp_ - timestamp;
        // 只在大幅回绕时清空缓冲区（超过100ms认为是异常）
        if (regression > 100) {
            LOG_WARNING("[AACEncoder] Large timestamp regression: " +
                        std::to_string(regression) + "ms, clearing buffer");
            input_buffer_.clear();
            first_frame_timestamp_ms_ = -1;  // 重置基准
        }
        // 小幅度回绕忽略，继续处理
    }
    last_audio_timestamp_ = timestamp;

    // 降低日志输出频率
    if (encode_count % 1000 == 0) {
        LOG_INFO("[AACEncoder] Audio encoding: count=" + std::to_string(encode_count) +
                 ", buffer size=" + std::to_string(input_buffer_.size()));
    }

    // 添加数据到输入缓冲区
    input_buffer_.append(data.constData(), data.size());

    // 防止缓冲区无限增长 - 限制最大5秒的音频数据
    size_t max_buffer_size = config_.sample_rate * sizeof(int16_t) * config_.channels * 5;
    if (input_buffer_.size() > static_cast<int>(max_buffer_size)) {
        LOG_WARNING("[AACEncoder] Input buffer too large (" + std::to_string(input_buffer_.size()) +
                   " bytes), clearing to prevent memory growth");
        input_buffer_.clear();
        first_frame_timestamp_ms_ = -1;
    }

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

    // 计算当前帧应该对应的时间戳（基于编码器帧大小）
    // 每帧1024采样，在48000Hz下约21.33ms
    int64_t frame_duration_ms = (codec_ctx_->frame_size * 1000) / codec_ctx_->sample_rate;

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

        // ✅ 修复：将毫秒时间戳转换为采样数作为 PTS
        // time_base 是 {1, sample_rate}，所以 PTS 应该是采样数
        // 例如：23ms @ 48000Hz = 23 * 48000 / 1000 = 1104 采样
        // 🔧 确保第一帧从 0 开始，避免播放器等待同步
        if (first_frame_timestamp_ms_ == -1) {
            first_frame_timestamp_ms_ = timestamp;
            LOG_INFO("[AACEncoder] First audio frame timestamp: " + std::to_string(timestamp) + "ms");
        }
        int64_t relative_timestamp_ms = timestamp - first_frame_timestamp_ms_;
        frame_->pts = (relative_timestamp_ms * codec_ctx_->sample_rate) / 1000;

        // 更新时间戳用于下一帧（每帧约21.33ms @ 48kHz）
        timestamp += frame_duration_ms;

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
                // 过滤掉已知的6字节异常帧（会导致杂音）
                if (packet_->size == 6) {
                    LOG_DEBUG("[AACEncoder] Filtered abnormal 6-byte AAC frame");
                } else {
                    // ✅ 设置 duration（AAC 编码器通常为 1024 采样）
                    if (packet_->duration <= 0) {
                        packet_->duration = codec_ctx_->frame_size > 0 ? codec_ctx_->frame_size : 1024;
                    }
                    // ✅ 使用编码器输出的 PTS（packet->pts），因为编码器可能会修改 PTS
                    // 例如 AAC 编码器可能有延迟或填充，导致输出 PTS 与输入不同
                    emit audio_encoded(packet_->data, packet_->size, packet_->pts);
                    frames_processed++;
                }
            }
            av_packet_unref(packet_);
        }
    }
}

// 转换输入数据（与原项目的 convertInputData 对应）
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

    // 🔧 只清零未使用的样本（如果转换的样本少于帧大小）
    // 这样可以避免预先清零导致的潜在问题
    if (ret < frame->nb_samples) {
        for (int ch = 0; ch < frame->ch_layout.nb_channels; ch++) {
            if (frame->data[ch]) {
                int unused_samples = frame->nb_samples - ret;
                float* dst = reinterpret_cast<float*>(frame->data[ch]) + ret;
                memset(dst, 0, unused_samples * sizeof(float));
            }
        }
    }

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
    last_audio_timestamp_ = -1;
    first_frame_timestamp_ms_ = -1;  // 重置第一帧时间戳基准

    LOG_INFO("[AACEncoder] Reset complete");
    return ErrorCode::SUCCESS;
}

} // namespace live_assistant
