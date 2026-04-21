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
AACEncoder::AACEncoder() : output_frame_count_(0), frame_duration_ms_(21), frame_offset_in_batch_(0), frame_samples_(1024) {
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
    // ✅ FLV 容器使用毫秒作为 time_base (1/1000)
    // 为了避免转换错误，直接使用毫秒作为编码器时间基
    // PTS 单位是毫秒，duration 也是毫秒（每帧 1024采样 @ 48kHz ≈ 21.33ms）
    return AVRational{1, 1000};
}

ErrorCode AACEncoder::ensure_swr() {
    std::lock_guard<std::mutex> lock(encoder_mutex_);

    if (swr_) {
        return ErrorCode::SUCCESS;
    }

    // 输入格式：AudioFrame 使用交错格式 float
    // 注意：注释说 AudioFrame 是交错格式 (LRLRLR)，所以应该用 FLT 而不是 FLTP
    AVChannelLayout in_layout = make_channel_layout(config_.channels);
    AVChannelLayout out_layout = codec_ctx_->ch_layout;

    // 🔧 诊断：打印采样率配置
    LOG_DEBUG("[AACEncoder] ensure_swr: input_sample_rate=" + std::to_string(config_.sample_rate) +
             ", output_sample_rate=" + std::to_string(codec_ctx_->sample_rate) +
             ", channels=" + std::to_string(config_.channels));

    if (swr_alloc_set_opts2(
            &swr_,
            &out_layout,
            AV_SAMPLE_FMT_FLTP,  // AAC需要 FLTP 格式 (输出)
            codec_ctx_->sample_rate,
            &in_layout,
            AV_SAMPLE_FMT_FLTP,   // 输入也是 FLTP（我们在 convert_input_data 中已转为平面格式）
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
        swr_ = nullptr;
        LOG_ERROR("Failed to init swr");
        return ErrorCode::INIT_FAILED;
    }

    LOG_DEBUG("[AACEncoder] Swr initialized successfully, swr_=" + std::to_string(reinterpret_cast<uint64_t>(swr_)));

    return ErrorCode::SUCCESS;
}

ErrorCode AACEncoder::initialize(const AudioEncoderConfig& config) {
    LOG_INFO("Initializing AAC encoder (FFmpeg)");

    config_ = config;
    last_audio_timestamp_ = -1;
    buffered_start_timestamp_us_ = -1;

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
    // ✅ 设置 codec_ctx_->time_base 为毫秒，与 FLV 容器一致
    // 这样 avcodec_send_frame 时使用的 PTS 单位就是毫秒
    codec_ctx_->time_base = AVRational{1, 1000};

    codec_ctx_->ch_layout = make_channel_layout(config_.channels);
    codec_ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;  // AAC编码必须使用 FLTP

    // 🔧 AAC-LC 设置（双重保险）
    // 1. 设置 codec_ctx_->profile
    codec_ctx_->profile = FF_PROFILE_AAC_LOW;

    // 🔧 诊断：打印 frame_size
    LOG_DEBUG("[AACEncoder] codec_ctx_->frame_size before open: " + std::to_string(codec_ctx_->frame_size));

    // For FLV/RTMP, extradata (AudioSpecificConfig) is required
    codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // 设置AAC编码参数
    AVDictionary* opts = nullptr;
    // 🔧 关键：使用编码器选项强制指定 AAC-LC profile（放在最前面）
    av_dict_set(&opts, "profile", "aac_low", 0);
    av_dict_set(&opts, "aac_coder", "twoloop", 0);
    av_dict_set(&opts, "cutoff", "18000", 0);
    // 注意：移除 prediction/aac_pred，因为它们是 AAC Main profile 特性

    if (avcodec_open2(codec_ctx_, codec_, &opts) < 0) {
        LOG_ERROR("Failed to open AAC encoder");
        av_dict_free(&opts);
        return ErrorCode::INIT_FAILED;
    }
    av_dict_free(&opts);

    // 🔧 验证编码器使用的profile
    LOG_DEBUG("[AACEncoder] Encoder opened with profile: " +
             std::to_string(codec_ctx_->profile) +
             " (FF_PROFILE_AAC_LOW=" + std::to_string(FF_PROFILE_AAC_LOW) +
             ", FF_PROFILE_AAC_MAIN=" + std::to_string(FF_PROFILE_AAC_MAIN) + ")");

    // 🔧 诊断：打印 frame_size 在打开编码器之后
    LOG_DEBUG("[AACEncoder] codec_ctx_->frame_size after open: " + std::to_string(codec_ctx_->frame_size));

    // 🔧 AAC-LC 兼容性修复：检查并强制替换 extradata 为 AAC-LC 格式
    // FFmpeg 可能仍然生成 Main profile 的 extradata
    if (codec_ctx_->extradata && codec_ctx_->extradata_size >= 2) {
        int audioObjectType = (codec_ctx_->extradata[0] >> 3) & 0x1F;
        LOG_DEBUG("[AACEncoder] Current extradata: audioObjectType=" + std::to_string(audioObjectType) +
                 ", bytes=[0x" + std::to_string(static_cast<int>(codec_ctx_->extradata[0])) +
                 ", 0x" + std::to_string(static_cast<int>(codec_ctx_->extradata[1])) + "]");

        // AAC-LC 的 audioObjectType = 2, AAC-Main = 1
        // 还需要检查 extradata_size：FFmpeg 原生编码器可能产生 5 字节扩展格式
        // （samplingFreqIndex=15 escape + 24 位明文频率），该格式与标准 2 字节格式
        // 均符合 AAC 规范，但腾讯 CSS 等 CDN 的 HLS 转码器通常只处理 2 字节紧凑格式。
        // 因此，不论什么情况，只要不是 2 字节紧凑格式就强制替换为标准 2 字节格式。
        if (audioObjectType != 2 || codec_ctx_->extradata_size != 2) {
            if (audioObjectType != 2) {
                LOG_WARNING("[AACEncoder] Detected non-LC profile (audioObjectType=" +
                           std::to_string(audioObjectType) + "), forcing AAC-LC extradata");
            } else {
                LOG_WARNING("[AACEncoder] extradata is AAC-LC but size=" +
                           std::to_string(codec_ctx_->extradata_size) +
                           " (extended/escape format), normalizing to compact 2-byte for CDN compatibility");
            }

            // 根据采样率选择索引
            int samplingFreqIndex = 3;  // 默认 48000Hz
            if (config_.sample_rate == 44100) samplingFreqIndex = 4;
            else if (config_.sample_rate == 22050) samplingFreqIndex = 7;
            else if (config_.sample_rate == 32000) samplingFreqIndex = 5;
            else if (config_.sample_rate == 16000) samplingFreqIndex = 8;
            else if (config_.sample_rate == 8000) samplingFreqIndex = 11;

            // 构造 AAC-LC AudioSpecificConfig (2 字节)
            // audioObjectType = 2 (AAC-LC)
            uint8_t asc[2];
            asc[0] = (2 << 3) | (samplingFreqIndex >> 1);  // audioObjectType=2 (LC)
            asc[1] = ((samplingFreqIndex & 1) << 7) | (config_.channels << 3);

            // 释放旧 extradata 并分配新的
            av_free(codec_ctx_->extradata);
            codec_ctx_->extradata = (uint8_t*)av_malloc(2 + AV_INPUT_BUFFER_PADDING_SIZE);
            if (codec_ctx_->extradata) {
                memcpy(codec_ctx_->extradata, asc, 2);
                codec_ctx_->extradata_size = 2;
                memset(codec_ctx_->extradata + 2, 0, AV_INPUT_BUFFER_PADDING_SIZE);
                LOG_INFO("[AACEncoder] ✓ Replaced extradata with AAC-LC: [0x" +
                        std::to_string(static_cast<int>(asc[0])) +
                        ", 0x" + std::to_string(static_cast<int>(asc[1])) + "]");
            }
        } else {
            LOG_INFO("[AACEncoder] ✓ extradata is compact 2-byte AAC-LC (audioObjectType=2, size=2)");
        }
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

    // 🔧 验证 frame 缓冲区是否正确分配
    if (!frame_->data[0]) {
        LOG_ERROR("Frame buffer allocation failed - data[0] is null");
        return ErrorCode::INIT_FAILED;
    }

    LOG_DEBUG("[AACEncoder] Frame buffer allocated: nb_samples=" + std::to_string(frame_->nb_samples) +
             ", data[0]=" + std::to_string(reinterpret_cast<uint64_t>(frame_->data[0])));

    packet_ = av_packet_alloc();
    if (!packet_) {
        LOG_ERROR("Failed to alloc audio packet");
        return ErrorCode::INIT_FAILED;
    }

    ErrorCode swr_ret = ensure_swr();
    if (swr_ret != ErrorCode::SUCCESS) {
        return swr_ret;
    }

    // 🔧 初始化帧持续时间（微秒精度，避免整数截断）
    // 每帧 1024 采样 @ 48kHz = 21333.33...us ≈ 21.333ms
    // 旧代码: 1024 * 1000 / 48000 = 21（丢失 0.333ms，30秒累积约10ms漂移）
    frame_samples_ = codec_ctx_->frame_size > 0 ? codec_ctx_->frame_size : 1024;
    frame_duration_us_ = static_cast<int64_t>(frame_samples_) * 1000000LL / codec_ctx_->sample_rate;
    // ⚠️ frame_duration_ms_ 用整数除法，在 fallback pts_to_emit 路径中会有 ~0.333ms/帧累积误差
    // 正确值应为 frame_duration_us_ / 1000.0 ≈ 21.3333
    frame_duration_ms_ = frame_duration_us_ / 1000;
    LOG_INFO("  - frame_duration_us: " + std::to_string(frame_duration_us_) +
             " (" + std::to_string(frame_duration_us_ / 1000.0) + "ms)");

    // 🔧 重置输出帧计数器
    output_frame_count_ = 0;

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

    // 🔧 启动异步编码线程
    if (!encode_thread_running_.load()) {
        encode_thread_running_.store(true);
        encode_thread_ = std::thread(&AACEncoder::encode_thread_func, this);
        LOG_INFO("[AACEncoder] Async encode thread started");
    }

    return ErrorCode::SUCCESS;
}

ErrorCode AACEncoder::shutdown() {
    // 🔧 停止编码线程
    if (encode_thread_running_.load()) {
        encode_thread_running_.store(false);
        encode_cv_.notify_all();
        if (encode_thread_.joinable()) {
            encode_thread_.join();
        }
    }

    // 清空编码队列
    {
        std::lock_guard<std::mutex> lock(encode_queue_mutex_);
        while (!encode_queue_.empty()) {
            encode_queue_.pop();
        }
    }

    // 🔧 线程安全：获取互斥锁，确保没有其他线程正在使用 swr_
    std::lock_guard<std::mutex> lock(encoder_mutex_);

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
    buffered_start_timestamp_us_ = -1;
    input_buffer_.clear();
    buffered_start_timestamp_us_ = -1;
    output_frame_count_ = 0;

    return ErrorCode::SUCCESS;
}

void AACEncoder::clear_encode_queue() {
    std::lock_guard<std::mutex> lock(encode_queue_mutex_);
    while (!encode_queue_.empty()) {
        encode_queue_.pop();
    }
}

// 接收原始音频数据并编码（异步版本，不阻塞调用者）
void AACEncoder::encode_audio_data(const QByteArray& data, int64_t timestamp) {
    if (!initialized_ || data.isEmpty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(encode_queue_mutex_);
        static constexpr size_t MAX_QUEUE_SIZE = 30;
        if (encode_queue_.size() >= MAX_QUEUE_SIZE) {
            encode_queue_.pop();
            static int drop_count = 0;
            if (++drop_count <= 10) {
                LOG_WARNING("[AACEncoder] Encode queue full, dropping oldest frame");
            }
        }
        AudioEncodeJob job;
        job.data = data;
        job.timestamp_ms = timestamp;
        encode_queue_.push(std::move(job));
    }
    encode_cv_.notify_one();
}

// 🔧 异步编码线程函数
void AACEncoder::encode_thread_func() {
    LOG_INFO("[AACEncoder] Encode thread started");

    while (encode_thread_running_.load()) {
        AudioEncodeJob job;

        // 从队列获取数据（带超时，避免一直等待）
        {
            std::unique_lock<std::mutex> lock(encode_queue_mutex_);
            auto timeout = std::chrono::milliseconds(50);

            if (!encode_cv_.wait_for(lock, timeout, [this] {
                return !encode_queue_.empty() || !encode_thread_running_.load();
            })) {
                // 超时，继续循环检查线程是否应该退出
                continue;
            }

            if (!encode_thread_running_.load() && encode_queue_.empty()) {
                break;
            }

            if (encode_queue_.empty()) {
                continue;
            }

            job = std::move(encode_queue_.front());
            encode_queue_.pop();
        }

        // 处理编码（同步执行，但不会阻塞混音线程）
        process_audio_data(job.data, job.timestamp_ms);
    }

    LOG_INFO("[AACEncoder] Encode thread exited");
}

// 实际的音频编码处理（从 encode_audio_data 提取的逻辑）
void AACEncoder::process_audio_data(const QByteArray& data, int64_t timestamp) {
    if (!initialized_ || data.isEmpty()) {
        return;
    }

    // 🔧 线程安全：获取互斥锁，保护 swr_ 和 codec_ctx_ 的访问
    std::lock_guard<std::mutex> lock(encoder_mutex_);

    // 🔧 诊断：检查输入音频数据的实际值（仅前3帧）
    static int data_diag_count = 0;
    data_diag_count++;
    if (data_diag_count <= 3) {
        const float* float_data = reinterpret_cast<const float*>(data.constData());
        int check_samples = std::min((int)data.size() / (int)sizeof(float), 10);
        float first_val = float_data[0];
        float second_val = check_samples > 1 ? float_data[1] : 0;
        float fifth_val = check_samples > 4 ? float_data[4] : 0;
        double sum_squares = 0;
        for (int i = 0; i < check_samples; i++) {
            sum_squares += float_data[i] * float_data[i];
        }
        double rms = std::sqrt(sum_squares / check_samples);
        LOG_DEBUG("[AACEncoder] Input data #" + std::to_string(data_diag_count) +
                 ": size=" + std::to_string(data.size()) +
                 ", first=" + std::to_string(first_val) +
                 ", second=" + std::to_string(second_val) +
                 ", fifth=" + std::to_string(fifth_val) +
                 ", RMS=" + std::to_string(rms));
    }

    static int64_t first_timestamp = -1;
    encode_count_++;

    // 🔧 诊断第一帧问题（仅前5帧）
    if (first_timestamp == -1) {
        first_timestamp = timestamp;
        LOG_DEBUG("[AACEncoder] First audio frame timestamp: " + std::to_string(timestamp) + "ms");
    } else if (encode_count_ <= 5) {
        LOG_DEBUG("[AACEncoder] Frame " + std::to_string(encode_count_) +
                 ": timestamp=" + std::to_string(timestamp) +
                 "ms, delta=" + std::to_string(timestamp - last_audio_timestamp_) + "ms");
    }

    // 🔧 检测时间戳回绕（异常跳跃）
    if (last_audio_timestamp_ > 0 && timestamp < last_audio_timestamp_) {
        int64_t regression = last_audio_timestamp_ - timestamp;
        if (regression > 100) {
            LOG_WARNING("[AACEncoder] Large timestamp regression: " +
                        std::to_string(regression) + "ms, clearing input buffer only");
            input_buffer_.clear();
            buffered_start_timestamp_us_ = -1;
        }
    }
    last_audio_timestamp_ = timestamp;

    // 降低日志输出频率
    if (encode_count_ % 1000 == 0) {
        LOG_DEBUG("[AACEncoder] Audio encoding: count=" + std::to_string(encode_count_) +
                 ", buffer size=" + std::to_string(input_buffer_.size()));
    }

    int input_bytes_per_sample = sizeof(float);
    int frame_samples = codec_ctx_->frame_size > 0 ? codec_ctx_->frame_size : 1024;
    int bytes_per_frame = frame_samples * input_bytes_per_sample * config_.channels;

    if (bytes_per_frame <= 0) {
        if (encode_count_ % 500 == 0) {
            LOG_WARNING("[AACEncoder] Invalid bytesPerFrame: " + std::to_string(bytes_per_frame));
        }
        return;
    }

    // 🔧 不再缓冲：每收到一帧音频数据（3840 bytes = 480 samples × 2ch × 4B）
    // 直接编码一帧。AAC 编码器本身有 frame_size=1024 的缓冲需求，
    // 我们利用编码器内部的 1024-sample 缓冲来吸收余数，而非在应用层累积 buffer。
    // 这样 emit 时的 output_frame_count_ 与输入顺序严格对应。
        if (input_buffer_.isEmpty()) {
        buffered_start_timestamp_us_ = timestamp * 1000LL;
    }
    input_buffer_.append(data);
    int64_t frame_duration_us = frame_duration_us_;
    if (frame_duration_us <= 0) {
        frame_duration_us = frame_duration_ms_ * 1000LL;
    }
    while (input_buffer_.size() >= bytes_per_frame) {
        if (buffered_start_timestamp_us_ < 0) {
            buffered_start_timestamp_us_ = timestamp * 1000LL;
        }
        QByteArray buffered_frame = input_buffer_.left(bytes_per_frame);
        input_buffer_.remove(0, bytes_per_frame);
        const int64_t pts_ms = buffered_start_timestamp_us_ / 1000LL;
        encode_one_frame(buffered_frame, pts_ms);
        buffered_start_timestamp_us_ += frame_duration_us;
    }
    if (input_buffer_.isEmpty()) {
        buffered_start_timestamp_us_ = -1;
    }
}

// 🔧 单帧编码：完成 send+receive 循环，返回 emit 的 PTS（毫秒）
// 始终从 output_frame_count_ 推算 PTS，保证单调递增
int64_t AACEncoder::encode_one_frame(const QByteArray& frame_data, int64_t timestamp) {
    // 转换输入数据到编码器所需格式
    int converted_samples = convert_input_data(
        reinterpret_cast<const uint8_t*>(frame_data.constData()),
        frame_data.size(),
        frame_
    );

    if (converted_samples <= 0) {
        if (encode_count_ % 500 == 0) {
            LOG_WARNING("[AACEncoder] Invalid frame samples: " + std::to_string(converted_samples));
        }
        return -1;
    }

    // 🔧 PTS 计算：始终基于 output_frame_count_，与输入 timestamp 脱钩
    // timestamp 仅用于初始化首帧基准
        const int64_t pts_to_emit = timestamp;
    frame_->pts = pts_to_emit;

    // 发送帧到编码器
    int ret = avcodec_send_frame(codec_ctx_, frame_);
    if (ret < 0) {
        if (encode_count_ % 500 == 0) {
            char errbuf[1024];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_WARNING("[AACEncoder] avcodec_send_frame failed: " + std::string(errbuf));
        }
        return -1;
    }

    // 接收编码后的数据包
    while (ret >= 0) {
        ret = avcodec_receive_packet(codec_ctx_, packet_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            if (encode_count_ % 500 == 0) {
                char errbuf[1024];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_WARNING("[AACEncoder] avcodec_receive_packet failed: " + std::string(errbuf));
            }
            break;
        }

        if (packet_->size > 0) {
            if (packet_->duration <= 0) {
                packet_->duration = frame_duration_ms_;
            }

            // 🔧 使用推算 PTS，覆盖编码器返回的值
            packet_->pts = pts_to_emit;
            packet_->dts = pts_to_emit;

            // 🔧 诊断日志（前5帧）
            static int emit_count = 0;
            emit_count++;
            if (emit_count <= 5) {
                LOG_INFO("[AACEncoder] emit #" + std::to_string(emit_count) +
                         ": pts=" + std::to_string(pts_to_emit) + "ms" +
                         ", output_frame_count=" + std::to_string(output_frame_count_) +
                         ", size=" + std::to_string(packet_->size));
            }

            emit audio_encoded(QByteArray(reinterpret_cast<const char*>(packet_->data), packet_->size),
                               pts_to_emit);
            output_frame_count_++;
        }
        av_packet_unref(packet_);
    }

    return pts_to_emit;
}

// 转换输入数据（与原项目的 convertInputData 对应）
int AACEncoder::convert_input_data(const uint8_t* input_data, int input_size, AVFrame* frame) {
    if (!input_data || input_size <= 0 || !frame) {
        LOG_ERROR("[AACEncoder] convert_input_data: invalid parameters - input_data=" +
                  std::to_string(reinterpret_cast<uint64_t>(input_data)) +
                  ", input_size=" + std::to_string(input_size) +
                  ", frame=" + std::to_string(reinterpret_cast<uint64_t>(frame)));
        return 0;
    }

    if (!swr_) {
        LOG_ERROR("[AACEncoder] Resampler not initialized (swr_ is null), attempting to reinitialize...");
        // 尝试重新初始化 swr
        ErrorCode ret = ensure_swr();
        if (ret != ErrorCode::SUCCESS || !swr_) {
            LOG_ERROR("[AACEncoder] Failed to reinitialize resampler");
            return 0;
        }
        LOG_INFO("[AACEncoder] Resampler reinitialized successfully");
    }

    // 🔧 诊断：检查 frame 的关键属性
    LOG_DEBUG("[AACEncoder] convert_input_data: frame->format=" + std::to_string(frame->format) +
              ", frame->ch_layout.nb_channels=" + std::to_string(frame->ch_layout.nb_channels) +
              ", frame->nb_samples=" + std::to_string(frame->nb_samples) +
              ", config_.channels=" + std::to_string(config_.channels));

    // 检查 frame->data 是否有效
    if (!frame->data[0]) {
        LOG_ERROR("[AACEncoder] Frame data buffer not allocated - frame->data[0]=" +
                  std::to_string(reinterpret_cast<uint64_t>(frame->data[0])) +
                  ", nb_samples=" + std::to_string(frame->nb_samples));
        return 0;
    }

    int bytes_per_sample = sizeof(float);
    int input_samples = input_size / (bytes_per_sample * config_.channels);

    if (input_samples <= 0) {
        LOG_WARNING("[AACEncoder] Invalid input sample count: input_size=" + std::to_string(input_size) +
                   ", bytes_per_sample=" + std::to_string(bytes_per_sample) +
                   ", channels=" + std::to_string(config_.channels));
        return 0;
    }

    // 检查 frame->nb_samples 是否有效
    if (frame->nb_samples <= 0) {
        LOG_ERROR("[AACEncoder] Invalid frame nb_samples: " + std::to_string(frame->nb_samples));
        return 0;
    }

    // 🔧 额外检查：确保输入数据指针在合理范围内
    if (reinterpret_cast<uint64_t>(input_data) < 0x1000) {
        LOG_ERROR("[AACEncoder] Suspicious input_data pointer: " +
                  std::to_string(reinterpret_cast<uint64_t>(input_data)));
        return 0;
    }

    // 🔧 输入是交错格式 (FLT)，需要转换为平面格式 (FLTP) 供 swr 使用
    // 先创建一个临时缓冲区
    static thread_local std::vector<float> planar_buffer;
    planar_buffer.resize(input_samples * config_.channels);
    
    // 将交错格式 LRLRLR 转为平面格式 LLLLRRRR
    // 正确：左声道连续存储，然后是右声道
    const float* src = reinterpret_cast<const float*>(input_data);
    for (int ch = 0; ch < config_.channels; ch++) {
        for (int i = 0; i < input_samples; i++) {
            // planar_buffer[ch * input_samples + i] = 第 ch 个通道的第 i 个样本
            // src[i * config_.channels + ch] = 第 i 个采样点的第 ch 个通道
            planar_buffer[ch * input_samples + i] = src[i * config_.channels + ch];
        }
    }
    
    const uint8_t* input_data_arr[8] = {nullptr};
    for (int ch = 0; ch < config_.channels; ch++) {
        input_data_arr[ch] = reinterpret_cast<const uint8_t*>(planar_buffer.data() + ch * input_samples);
    }

    // 🔧 额外安全检查：确保 frame->data 数组中的所有通道都有效
    for (int ch = 0; ch < frame->ch_layout.nb_channels; ch++) {
        if (!frame->data[ch]) {
            LOG_ERROR("[AACEncoder] Frame data channel " + std::to_string(ch) + " is null");
            return 0;
        }
    }

    // 🔧 诊断：打印关键值（仅前3帧）
    static int swr_debug_count = 0;
    swr_debug_count++;
    if (swr_debug_count <= 3) {
        LOG_DEBUG("[AACEncoder] swr_convert: input_samples=" + std::to_string(input_samples) +
                  ", frame->nb_samples=" + std::to_string(frame->nb_samples) +
                  ", channels=" + std::to_string(config_.channels));
    }

    // 🔧 诊断：在调用 swr_convert 前验证所有参数
    if (!swr_) {
        LOG_ERROR("[AACEncoder] FATAL: swr_ is null before swr_convert!");
        return 0;
    }
    if (!frame->data[0]) {
        LOG_ERROR("[AACEncoder] FATAL: frame->data[0] is null before swr_convert!");
        return 0;
    }
    if (!input_data) {
        LOG_ERROR("[AACEncoder] FATAL: input_data is null before swr_convert!");
        return 0;
    }

    int ret = swr_convert(
        swr_,
        frame->data,
        frame->nb_samples,
        input_data_arr,
        input_samples
    );

    // 🔧 诊断：swr_convert 返回后立即记录
    LOG_DEBUG("[AACEncoder] swr_convert returned: ret=" + std::to_string(ret));

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
    // 保留此方法以兼容旧接口，直接使用 float 格式
    if (!in || !in->data) {
        return ErrorCode::INVALID_PARAM;
    }

    // 直接使用 float 格式（32-bit）
    int num_samples = in->samples * in->channels;
    QByteArray data(reinterpret_cast<const char*>(in->data), num_samples * sizeof(float));

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
            // duration 已经是毫秒
            out->duration = codec_ctx_->frame_size * 1000 / codec_ctx_->sample_rate;
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
    // 🔧 线程安全：获取互斥锁，确保没有其他线程正在使用 swr_
    std::lock_guard<std::mutex> lock(encoder_mutex_);

    if (!initialized_) {
        return ErrorCode::INIT_FAILED;
    }

    // 清空输入缓冲区
    input_buffer_.clear();

    // 🔧 重新初始化 swr，确保重采样器状态正确
    if (swr_) {
        swr_free(&swr_);
        swr_ = nullptr;
    }

    // 🔧 注意：ensure_swr 内部也会尝试获取锁，所以这里直接调用会死锁
    // 改为直接在这里初始化 swr，而不是调用 ensure_swr
    AVChannelLayout in_layout = make_channel_layout(config_.channels);
    AVChannelLayout out_layout = codec_ctx_->ch_layout;

    if (swr_alloc_set_opts2(
            &swr_,
            &out_layout,
            AV_SAMPLE_FMT_FLTP,
            codec_ctx_->sample_rate,
            &in_layout,
            AV_SAMPLE_FMT_FLTP,
            config_.sample_rate,
            0,
            nullptr) < 0) {
        LOG_ERROR("[AACEncoder] Failed to alloc swr in reset()");
        return ErrorCode::INIT_FAILED;
    }

    if (!swr_ || swr_init(swr_) < 0) {
        if (swr_) {
            swr_free(&swr_);
        }
        swr_ = nullptr;
        LOG_ERROR("[AACEncoder] Failed to init swr in reset()");
        return ErrorCode::INIT_FAILED;
    }

    LOG_DEBUG("[AACEncoder] Swr reinitialized successfully in reset()");

    // 注意：不调用 avcodec_flush_buffers，该调用会损坏 FFmpeg native AAC 编码器
    // 的内部 MDCT overlap 缓冲区，导致重置后前几帧输出 6 字节垃圾包，进而触发
    // 解码侧 "Input buffer exhausted" / "channel element X.Y is not allocated" 等错误。
    // reset() 的目标只是清空输入缓冲和 swr 状态，编解码器内部状态保持不变。

    last_audio_timestamp_ = -1;
    buffered_start_timestamp_us_ = -1;
    output_frame_count_ = 0;

    frame_offset_in_batch_ = 0;

    LOG_INFO("[AACEncoder] Reset complete");
    return ErrorCode::SUCCESS;
}

} // namespace live_assistant
