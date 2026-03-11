#include "media_pipeline/media_file_source.h"
#include "video_engine/video_engine.h"
#include "audio_engine/audio_engine.h"
#include "common/log.h"
#include <QFile>
#include <climits>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
}

namespace live_assistant {

// ============================================================================
// 构造/析构
// ============================================================================

MediaFileSource::MediaFileSource(const std::string& id, std::shared_ptr<InsertFileItem> file_item)
    : Source(id, Type::FILE_SOURCE), file_item_(file_item) {
    LOG_INFO("[MediaFileSource] Created: " + id);
}

MediaFileSource::~MediaFileSource() {
    shutdown();
    LOG_INFO("[MediaFileSource] Destroyed: " + id_);
}

// ============================================================================
// 初始化
// ============================================================================

bool MediaFileSource::initialize() {
    if (!file_item_) {
        LOG_ERROR("[MediaFileSource] No file item provided");
        return false;
    }

    QString localPath = file_item_->getLocalCachePath();
    if (!QFile::exists(localPath)) {
        LOG_ERROR("[MediaFileSource] File not found: " + localPath.toStdString());
        return false;
    }

    if (!initializeReader()) {
        return false;
    }

    if (!initializeDecoder()) {
        shutdownReader();
        return false;
    }

    LOG_INFO("[MediaFileSource] Initialized: " + file_item_->fileName.toStdString() +
             " (" + std::to_string(width_) + "x" + std::to_string(height_) +
             " @ " + std::to_string(video_fps_) + "fps)");
    return true;
}

bool MediaFileSource::start() {
    if (running_.exchange(true)) {
        LOG_WARNING("[MediaFileSource] Already running");
        return true;
    }

    reader_finished_ = false;
    finished_ = false;

    // 启动读取线程
    reader_thread_ = std::thread(&MediaFileSource::readerThreadFunc, this);
    LOG_INFO("[MediaFileSource] Reader thread started");

    // 启动调度线程
    scheduler_thread_ = std::thread(&MediaFileSource::schedulerThreadFunc, this);
    LOG_INFO("[MediaFileSource] Scheduler thread started");

    LOG_INFO("[MediaFileSource] Playback started");
    return true;
}

bool MediaFileSource::stop() {
    if (!running_.exchange(false)) {
        return true;
    }

    LOG_INFO("[MediaFileSource] Stopping playback...");

    // 通知所有等待的线程
    video_packet_cv_.notify_all();
    audio_packet_cv_.notify_all();

    // 等待线程结束
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
    if (scheduler_thread_.joinable()) {
        scheduler_thread_.join();
    }

    LOG_INFO("[MediaFileSource] Playback stopped");
    return true;
}

bool MediaFileSource::shutdown() {
    stop();
    shutdownDecoder();
    shutdownReader();
    return true;
}

// ============================================================================
// 帧获取接口
// ============================================================================

std::shared_ptr<AudioFrame> MediaFileSource::get_audio_frame() {
    std::unique_lock<std::mutex> lock(audio_frame_mutex_);
    if (audio_frame_queue_.empty()) {
        return nullptr;
    }
    
    // 获取队头的帧（FIFO）
    auto frame = audio_frame_queue_.front();
    
    // 获取后删除该帧
    audio_frame_queue_.pop();
    
    return frame;
}

std::shared_ptr<VideoFrame> MediaFileSource::get_video_frame() {
    std::unique_lock<std::mutex> lock(video_frame_mutex_);
    return video_frame_queue_.empty() ? nullptr : video_frame_queue_.back();
}

// ============================================================================
// Reader 线程 - 读取文件数据包
// ============================================================================

bool MediaFileSource::initializeReader() {
    QString localPath = file_item_->getLocalCachePath();

    // 打开输入文件
    if (avformat_open_input(&format_ctx_, localPath.toUtf8().constData(), nullptr, nullptr) < 0) {
        LOG_ERROR("[MediaFileSource] Failed to open input file");
        return false;
    }

    // 获取流信息
    if (avformat_find_stream_info(format_ctx_, nullptr) < 0) {
        LOG_ERROR("[MediaFileSource] Failed to find stream info");
        avformat_close_input(&format_ctx_);
        return false;
    }

    // 查找视频流和音频流
    for (unsigned int i = 0; i < format_ctx_->nb_streams; i++) {
        AVStream* stream = format_ctx_->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_idx_ == -1) {
            video_stream_idx_ = i;
            width_ = stream->codecpar->width;
            height_ = stream->codecpar->height;

            // 计算帧率
            AVRational frame_rate = av_guess_frame_rate(format_ctx_, stream, nullptr);
            if (frame_rate.den > 0) {
                video_fps_ = av_q2d(frame_rate);
            }
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_idx_ == -1) {
            audio_stream_idx_ = i;
            audio_sample_rate_ = stream->codecpar->sample_rate;
            audio_channels_ = stream->codecpar->ch_layout.nb_channels;
        }
    }

    // 获取时长
    if (format_ctx_->duration != AV_NOPTS_VALUE) {
        duration_ms_ = format_ctx_->duration * 1000 / AV_TIME_BASE;
    }

    LOG_INFO("[MediaFileSource] Reader initialized - V:" + std::to_string(video_stream_idx_) +
             " A:" + std::to_string(audio_stream_idx_) +
             " " + std::to_string(width_) + "x" + std::to_string(height_) +
             "@" + std::to_string(video_fps_) + "fps " +
             std::to_string(duration_ms_) + "ms");

    return true;
}

void MediaFileSource::readerThreadFunc() {
    LOG_INFO("[MediaFileSource-Reader] Thread started");

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        LOG_ERROR("[MediaFileSource-Reader] Failed to allocate packet");
        return;
    }

    int64_t packet_count = 0;
    int64_t last_log_time = 0;

    while (running_.load()) {
        // 检查队列是否已满
        {
            std::lock_guard<std::mutex> lock(video_packet_mutex_);
            if (video_packet_queue_.size() >= MAX_QUEUE_SIZE) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
        }

        int ret = av_read_frame(format_ctx_, packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                LOG_INFO("[MediaFileSource-Reader] EOF reached, total packets: " + std::to_string(packet_count));

                // 如果启用循环播放，不退出线程，而是重新开始读取
                if (loop_enabled_.load()) {
                    LOG_INFO("[MediaFileSource-Reader] Looping - seeking to beginning");

                    // 清空所有队列
                    {
                        std::lock_guard<std::mutex> vLock(video_packet_mutex_);
                        std::lock_guard<std::mutex> aLock(audio_packet_mutex_);
                        while (!video_packet_queue_.empty()) video_packet_queue_.pop();
                        while (!audio_packet_queue_.empty()) audio_packet_queue_.pop();
                    }

                    // 跳转到文件开头
                    av_seek_frame(format_ctx_, -1, 0, AVSEEK_FLAG_BACKWARD);

                    // 重置 packet_count 并继续
                    packet_count = 0;
                    last_log_time = 0;

                    // 继续读取
                    continue;
                } else {
                    reader_finished_ = true;
                }
            } else {
                LOG_ERROR("[MediaFileSource-Reader] Error reading frame: " + std::to_string(ret));
            }
            break;
        }

        packet_count++;

        // 每5秒记录一次队列状态
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - last_log_time > 5000) {
            std::lock_guard<std::mutex> vLock(video_packet_mutex_);
            std::lock_guard<std::mutex> aLock(audio_packet_mutex_);
            LOG_INFO("[MediaFileSource-Reader] Stats - packets:" + std::to_string(packet_count) +
                     " v_queue:" + std::to_string(video_packet_queue_.size()) +
                     " a_queue:" + std::to_string(audio_packet_queue_.size()));
            last_log_time = now;
        }

        // 处理视频包
        if (packet->stream_index == video_stream_idx_) {
            MediaPacket mediaPacket;
            mediaPacket.data = QByteArray(reinterpret_cast<const char*>(packet->data), packet->size);
            mediaPacket.pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
            mediaPacket.isKeyFrame = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            mediaPacket.isVideo = true;

            {
                std::lock_guard<std::mutex> lock(video_packet_mutex_);
                video_packet_queue_.push(std::move(mediaPacket));
            }
            video_packet_cv_.notify_one();
        }
        // 处理音频包
        else if (packet->stream_index == audio_stream_idx_) {
            MediaPacket mediaPacket;
            mediaPacket.data = QByteArray(reinterpret_cast<const char*>(packet->data), packet->size);
            mediaPacket.pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
            mediaPacket.isVideo = false;

            {
                std::lock_guard<std::mutex> lock(audio_packet_mutex_);
                audio_packet_queue_.push(std::move(mediaPacket));
            }
            audio_packet_cv_.notify_one();
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    reader_finished_ = true;
    LOG_INFO("[MediaFileSource-Reader] Thread exiting");
}

void MediaFileSource::shutdownReader() {
    if (format_ctx_) {
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
    }
    LOG_INFO("[MediaFileSource] Reader shutdown complete");
}

// ============================================================================
// Decoder & Scheduler 线程 - OBS风格的调度逻辑
// ============================================================================

bool MediaFileSource::initializeDecoder() {
    // 初始化视频解码器
    if (video_stream_idx_ >= 0) {
        AVStream* stream = format_ctx_->streams[video_stream_idx_];
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) {
            LOG_ERROR("[MediaFileSource] Failed to find video codec");
            return false;
        }

        video_codec_ctx_ = avcodec_alloc_context3(codec);
        if (!video_codec_ctx_) {
            LOG_ERROR("[MediaFileSource] Failed to allocate video codec context");
            return false;
        }

        if (avcodec_parameters_to_context(video_codec_ctx_, stream->codecpar) < 0) {
            LOG_ERROR("[MediaFileSource] Failed to copy video codec params");
            avcodec_free_context(&video_codec_ctx_);
            return false;
        }

        if (avcodec_open2(video_codec_ctx_, codec, nullptr) < 0) {
            LOG_ERROR("[MediaFileSource] Failed to open video codec");
            avcodec_free_context(&video_codec_ctx_);
            return false;
        }

        video_frame_ = av_frame_alloc();
        if (!video_frame_) {
            LOG_ERROR("[MediaFileSource] Failed to allocate video frame");
            return false;
        }

        // 性能优化：直接转换为原始分辨率的 RGBA，避免额外缩放
        // 使用 SWS_POINT (点采样) 最快算法，质量略低但性能高
        sws_ctx_ = sws_getContext(
            width_, height_, video_codec_ctx_->pix_fmt,
            width_, height_, AV_PIX_FMT_RGBA,
            SWS_POINT, nullptr, nullptr, nullptr
        );

        LOG_INFO("[MediaFileSource] Video decoder initialized: " +
                 std::to_string(width_) + "x" + std::to_string(height_) +
                 " (direct RGBA conversion, no scaling)");
    }

    // 初始化音频解码器
    if (audio_stream_idx_ >= 0) {
        AVStream* stream = format_ctx_->streams[audio_stream_idx_];
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) {
            LOG_ERROR("[MediaFileSource] Failed to find audio codec");
            return false;
        }

        audio_codec_ctx_ = avcodec_alloc_context3(codec);
        if (!audio_codec_ctx_) {
            LOG_ERROR("[MediaFileSource] Failed to allocate audio codec context");
            return false;
        }

        if (avcodec_parameters_to_context(audio_codec_ctx_, stream->codecpar) < 0) {
            LOG_ERROR("[MediaFileSource] Failed to copy audio codec params");
            avcodec_free_context(&audio_codec_ctx_);
            return false;
        }

        if (avcodec_open2(audio_codec_ctx_, codec, nullptr) < 0) {
            LOG_ERROR("[MediaFileSource] Failed to open audio codec");
            avcodec_free_context(&audio_codec_ctx_);
            return false;
        }

        audio_frame_ = av_frame_alloc();
        if (!audio_frame_) {
            LOG_ERROR("[MediaFileSource] Failed to allocate audio frame");
            return false;
        }

        // 初始化音频重采样上下文
        AVChannelLayout out_ch_layout;
        av_channel_layout_default(&out_ch_layout, target_channels_);

        // 🔧 修复：输出 float 格式，与 AudioEngine 期望一致
        swr_alloc_set_opts2(&swr_ctx_,
            &out_ch_layout, AV_SAMPLE_FMT_FLT, target_sample_rate_,
            &audio_codec_ctx_->ch_layout, audio_codec_ctx_->sample_fmt, audio_codec_ctx_->sample_rate,
            0, nullptr);

        if (swr_ctx_ && swr_init(swr_ctx_) < 0) {
            LOG_ERROR("[MediaFileSource] Failed to initialize audio resampler");
            swr_free(&swr_ctx_);
        }

        LOG_INFO("[MediaFileSource] Audio decoder initialized: " +
                 std::to_string(target_sample_rate_) + "Hz " +
                 std::to_string(target_channels_) + "ch");
    }

    return true;
}

void MediaFileSource::schedulerThreadFunc() {
    LOG_INFO("[MediaFileSource-Scheduler] Thread started");

    // OBS风格调度：基于时间戳的精确同步
    double effective_fps = (video_fps_ > 0 && video_fps_ <= 120) ? video_fps_ : 30.0;
    const int64_t frame_interval_ns = static_cast<int64_t>(1000000000.0 / effective_fps);

    // 时间戳同步变量
    int64_t start_pts_ns = 0;           // 第一帧的PTS
    int64_t base_sys_time_ns = 0;       // 视频时钟基准（可能被 lag 重置）
    int64_t audio_base_sys_time_ns = 0; // 音频独立时钟基准（不随 lag 重置，确保音频节奏稳定）
    bool first_frame = true;
    bool timing_synced = false;         // 时间戳同步标志

    // 统计变量
    int decoded_frame_count = 0;
    int decoded_audio_count = 0;
    int64_t last_decode_time_ms = 0;
    int64_t last_stats_time_ms = 0;

    while (running_.load()) {
        // ------------------------------------------
        // 步骤1：解码一帧视频
        // ------------------------------------------
        bool video_decoded = false;

        if (video_stream_idx_ >= 0) {
            std::unique_lock<std::mutex> lock(video_packet_mutex_);

            // 等待数据（带超时）
            int retry_count = 0;
            while (video_packet_queue_.empty() && !reader_finished_.load()) {
                lock.unlock();
                if (retry_count < 100) {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                retry_count++;
                if (!running_.load()) goto thread_exit;
                lock.lock();
            }

            if (!video_packet_queue_.empty()) {
                MediaPacket packet = std::move(video_packet_queue_.front());
                size_t queue_size = video_packet_queue_.size();
                video_packet_queue_.pop();
                lock.unlock();

                AVPacket* avPacket = av_packet_alloc();
                avPacket->data = reinterpret_cast<uint8_t*>(packet.data.data());
                avPacket->size = packet.data.size();
                avPacket->pts = packet.pts;
                avPacket->dts = packet.pts;

                // 测量完整解码流程性能
                auto decode_start = std::chrono::high_resolution_clock::now();

                if (decodeVideoPacket(avPacket)) {
                    auto decode_mid = std::chrono::high_resolution_clock::now();

                    auto videoFrame = convertToVideoFrame(video_frame_);
                    auto decode_end = std::chrono::high_resolution_clock::now();

                    if (videoFrame) {
                        // 每60帧记录解码性能
                        static int perf_frame_count = 0;
                        static int64_t total_decode_us = 0;
                        static int64_t total_scale_us = 0;

                        perf_frame_count++;
                        auto decode_us = std::chrono::duration_cast<std::chrono::microseconds>(decode_mid - decode_start).count();
                        auto scale_us = std::chrono::duration_cast<std::chrono::microseconds>(decode_end - decode_mid).count();
                        total_decode_us += decode_us;
                        total_scale_us += scale_us;

                        if (perf_frame_count % 60 == 0) {
                            LOG_INFO("[MediaFileSource] Decode perf: decode=" + std::to_string(total_decode_us / 60) +
                                     "us scale=" + std::to_string(total_scale_us / 60) +
                                     "us total=" + std::to_string((total_decode_us + total_scale_us) / 60) + "us");
                            total_decode_us = 0;
                            total_scale_us = 0;
                        }
                        int64_t frame_pts_ns = videoFrame->timestamp_ms * 1000000;

                        // 初始化时间戳基准（第一帧）
                        if (first_frame) {
                            start_pts_ns = frame_pts_ns;
                            int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
                            base_sys_time_ns = now_ns;
                            audio_base_sys_time_ns = now_ns; // 音频独立时钟，不随视频 lag 重置
                            first_frame = false;

                            LOG_INFO("[MediaFileSource-Scheduler] First frame PTS: " +
                                     std::to_string(frame_pts_ns / 1000000) + "ms, interval: " +
                                     std::to_string(frame_interval_ns / 1000000) + "ms");
                        }

                        // 计算时间偏移
                        int64_t current_sys_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        int64_t elapsed_ns = current_sys_time_ns - base_sys_time_ns;
                        int64_t target_ns = frame_pts_ns - start_pts_ns;

                        decoded_frame_count++;
                        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        int64_t delta_ms = (last_decode_time_ms > 0) ? (now_ms - last_decode_time_ms) : 0;
                        last_decode_time_ms = now_ms;

                        // 每60帧或前5帧记录详细日志
                        if (decoded_frame_count <= 5 || (decoded_frame_count % 60 == 0)) {
                            LOG_INFO("[MediaFileSource-Scheduler] Frame #" + std::to_string(decoded_frame_count) +
                                     " pts=" + std::to_string(videoFrame->timestamp_ms) + "ms" +
                                     " target=" + std::to_string(target_ns / 1000000) + "ms" +
                                     " elapsed=" + std::to_string(elapsed_ns / 1000000) + "ms" +
                                     " delta=" + std::to_string(delta_ms) + "ms" +
                                     " q=" + std::to_string(queue_size) +
                                     " key=" + std::to_string(packet.isKeyFrame ? 1 : 0) +
                                     " sync=" + std::to_string(timing_synced ? 1 : 0));
                        }

                        // 每5秒统计一次性能
                        if (now_ms - last_stats_time_ms > 5000) {
                            double actual_fps = decoded_frame_count * 1000.0 / (elapsed_ns / 1000000.0);
                            LOG_INFO("[MediaFileSource-Scheduler] Stats - frames:" + std::to_string(decoded_frame_count) +
                                     " audio:" + std::to_string(decoded_audio_count) +
                                     " fps:" + std::to_string(actual_fps) +
                                     " q:" + std::to_string(queue_size));
                            last_stats_time_ms = now_ms;
                        }

                        // 将解码后的帧放入队列（OBS风格：帧缓冲）
                        {
                            std::lock_guard<std::mutex> frameLock(video_frame_mutex_);

                            // 如果队列满了，删除最旧的帧
                            if (video_frame_queue_.size() >= MAX_VIDEO_FRAME_QUEUE_SIZE) {
                                video_frame_queue_.pop_front();
                            }

                            // 添加新帧到队列末尾
                            video_frame_queue_.push_back(videoFrame);
                        }

                        // 从队列中选择最合适的帧进行输出（始终基于 PTS 时间戳）
                        std::shared_ptr<VideoFrame> frame_to_output = nullptr;
                        {
                            std::lock_guard<std::mutex> frameLock(video_frame_mutex_);

                            if (video_frame_queue_.empty()) {
                                continue;
                            }

                            // 从队列中找到最接近目标时间的帧（PTS 不超过目标优先）
                            int64_t target_ms = target_ns / 1000000;
                            int64_t best_distance = INT64_MAX;
                            auto best_frame = video_frame_queue_.begin();
                            bool found_frame = false;

                            for (auto it = video_frame_queue_.begin(); it != video_frame_queue_.end(); ++it) {
                                int64_t frame_distance = std::abs((*it)->timestamp_ms - target_ms);
                                if ((*it)->timestamp_ms <= target_ms && frame_distance < best_distance) {
                                    best_distance = frame_distance;
                                    best_frame = it;
                                    found_frame = true;
                                }
                            }

                            // 如果没有找到 PTS <= 目标的帧（解码还在追赶），使用最新帧
                            if (!found_frame && !video_frame_queue_.empty()) {
                                best_frame = std::prev(video_frame_queue_.end());
                            }

                            frame_to_output = *best_frame;

                            // 清理已输出帧之前的旧帧
                            while (!video_frame_queue_.empty() &&
                                   video_frame_queue_.front()->timestamp_ms < frame_to_output->timestamp_ms) {
                                video_frame_queue_.pop_front();
                            }
                        }

                        // 输出选定的帧
                        if (frame_to_output) {
                            if (frame_ready_callback_) {
                                frame_ready_callback_(frame_to_output);
                            }

                            // 更新当前位置
                            if (format_ctx_ && video_stream_idx_ >= 0) {
                                AVStream* stream = format_ctx_->streams[video_stream_idx_];
                                if (packet.pts != AV_NOPTS_VALUE) {
                                    int64_t pos = packet.pts * av_q2d(stream->time_base) * 1000;
                                    current_position_ms_.store(pos);
                                }
                            }
                        }

                        video_decoded = true;
                    }
                }
                av_packet_free(&avPacket);
            } else {
                lock.unlock();
            }
        }

        // ------------------------------------------
        // 步骤2：解码音频（基于 PTS 节奏控制，独立于视频时钟）
        //
        // 问题背景：
        //   - 视频 30fps（33ms/帧），音频 48000/1024 ≈ 46.9fps（21ms/帧）
        //   - 固定每帧处理 2 个音频包 → 音频以 42ms/33ms = 1.27x 实时速度生产
        //   - 音频超前于实时播放时间 → AudioEngine media_queue_ 溢出 → 丢帧 → 卡顿
        //
        // 修复策略：
        //   - 用独立音频时钟（audio_base_sys_time_ns）计算已过去的实际播放时长
        //   - 只要"下一帧音频 PTS ≤ 实际已播放时长 + 预读缓冲（100ms）"才生产
        //   - 自动收敛到 ~1.57 帧/33ms（21ms 和 33ms 的最简整数调度）
        //   - 与视频 lag 重置无关（audio_base_sys_time_ns 从不被修改）
        // ------------------------------------------
        if (audio_stream_idx_ >= 0 && !first_frame) {
            static int audio_perf_count = 0;
            static int64_t total_audio_us = 0;

            // 音频每帧时长（ms），约 21.33ms
            const int64_t AUDIO_FRAME_MS = static_cast<int64_t>(TARGET_AUDIO_SAMPLES) * 1000LL
                                           / static_cast<int64_t>(target_sample_rate_);
            // 允许音频超前实时的最大量（预读缓冲，为混音线程提供稳定数据）
            const int64_t AUDIO_LOOKAHEAD_MS = 100;
            // 安全上限：每次循环最多生产 4 帧（防止某些极端情况无限循环）
            const int MAX_AUDIO_PER_ITER = 4;

            // 用音频独立时钟计算实际已播放时长
            int64_t audio_now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            int64_t audio_elapsed_ms = (audio_now_ns - audio_base_sys_time_ns) / 1000000;

            for (int audio_iter = 0; audio_iter < MAX_AUDIO_PER_ITER; ++audio_iter) {
                // 下一帧音频的 PTS（相对于播放开始）
                int64_t next_audio_pts_ms = static_cast<int64_t>(decoded_audio_count) * AUDIO_FRAME_MS;

                // 若音频已超前于实时 + 预读缓冲，停止生产，等实时赶上来
                if (next_audio_pts_ms > audio_elapsed_ms + AUDIO_LOOKAHEAD_MS) {
                    break;
                }

                auto audio_start = std::chrono::high_resolution_clock::now();

                MediaPacket packet;
                bool has_packet = false;
                {
                    std::lock_guard<std::mutex> lock(audio_packet_mutex_);
                    if (!audio_packet_queue_.empty()) {
                        packet = std::move(audio_packet_queue_.front());
                        audio_packet_queue_.pop();
                        has_packet = true;
                    }
                }
                if (!has_packet) break;

                AVPacket* avPacket = av_packet_alloc();
                avPacket->data = reinterpret_cast<uint8_t*>(packet.data.data());
                avPacket->size = packet.data.size();
                avPacket->pts = packet.pts;
                avPacket->dts = packet.pts;

                if (decodeAudioPacket(avPacket)) {
                    auto audioFrame = convertToAudioFrame(audio_frame_);

                    auto audio_end = std::chrono::high_resolution_clock::now();

                    if (audioFrame) {
                        int64_t audio_total_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            audio_end - audio_start).count();
                        audio_perf_count++;
                        total_audio_us += audio_total_us;

                        if (audio_perf_count >= 60) {
                            LOG_INFO("[MediaFileSource] Audio perf: avg=" +
                                     std::to_string(total_audio_us / 60) + "us");
                            audio_perf_count = 0;
                            total_audio_us = 0;
                        }

                        if (audio_ready_callback_) {
                            audio_ready_callback_(audioFrame);
                        }
                        decoded_audio_count++;
                    }
                }
                av_packet_free(&avPacket);
            }
        }

        // ------------------------------------------
        // 步骤3：检查结束
        // ------------------------------------------
        if (reader_finished_.load() && !loop_enabled_.load()) {
            LOG_INFO("[MediaFileSource-Scheduler] Playback finished");
            break;
        }

        // ------------------------------------------
        // 步骤 4：基于 PTS 的帧率控制（从第一帧开始即生效）
        // ------------------------------------------
        if (!first_frame && video_decoded) {
            int64_t current_sys_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            // 计算下一帧的预期时间
            int64_t next_target_ns = (decoded_frame_count * frame_interval_ns);
            int64_t sleep_ns = next_target_ns - (current_sys_time_ns - base_sys_time_ns);

            // 帧率控制策略：
            // 1. 超前超过 5ms：精确 sleep 等待
            // 2. 落后 0~5 帧：不 sleep，直接解码追赶
            // 3. 落后超过 5 帧：一次性重置时间基准，避免渐进调整引发振荡
            //    （渐进调整会导致正反馈：每次调整让下一帧 sleep 更多 → 更大 lag → 更多调整）
            if (sleep_ns > 5000000) { // 超过 5ms，精确等待
                std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
            } else if (sleep_ns < -(static_cast<int64_t>(frame_interval_ns) * 5)) {
                // 落后超过 5 帧（~165ms）：一次性重置时间基准
                // 重置后 sleep_ns ≈ 0，下一帧立即解码，不产生 sleep 积累
                int64_t lag_ms = (-sleep_ns) / 1000000;
                base_sys_time_ns = current_sys_time_ns
                    - static_cast<int64_t>(decoded_frame_count) * frame_interval_ns;
                LOG_INFO("[MediaFileSource-Scheduler] Lag " + std::to_string(lag_ms) +
                         "ms (>5 frames), resetting time base");
            }
            // 落后 0~5 帧：不 sleep，直接追赶（常见抖动，无需干预）
        }
    }

thread_exit:
    finished_ = true;
    LOG_INFO("[MediaFileSource-Scheduler] Thread exiting - total frames: " +
             std::to_string(decoded_frame_count));

    if (playback_finished_callback_) {
        playback_finished_callback_();
    }
}

void MediaFileSource::shutdownDecoder() {
    if (video_codec_ctx_) {
        avcodec_free_context(&video_codec_ctx_);
    }
    if (audio_codec_ctx_) {
        avcodec_free_context(&audio_codec_ctx_);
    }
    if (video_frame_) {
        av_frame_free(&video_frame_);
    }
    if (audio_frame_) {
        av_frame_free(&audio_frame_);
    }
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    if (swr_ctx_) {
        swr_free(&swr_ctx_);
    }
    LOG_INFO("[MediaFileSource] Decoder shutdown complete");
}

// ============================================================================
// 解码辅助函数
// ============================================================================

bool MediaFileSource::decodeVideoPacket(const AVPacket* packet) {
    if (!video_codec_ctx_ || !video_frame_) return false;

    int ret = avcodec_send_packet(video_codec_ctx_, packet);
    if (ret < 0) {
        return false;
    }

    ret = avcodec_receive_frame(video_codec_ctx_, video_frame_);
    if (ret < 0) {
        return false;
    }

    return true;
}

bool MediaFileSource::decodeAudioPacket(const AVPacket* packet) {
    if (!audio_codec_ctx_ || !audio_frame_) return false;

    int ret = avcodec_send_packet(audio_codec_ctx_, packet);
    if (ret < 0) {
        return false;
    }

    ret = avcodec_receive_frame(audio_codec_ctx_, audio_frame_);
    if (ret < 0) {
        return false;
    }

    return true;
}

std::shared_ptr<VideoFrame> MediaFileSource::convertToVideoFrame(AVFrame* frame) {
    if (!frame || !sws_ctx_) return nullptr;

    // 性能优化：直接使用原始分辨率，避免额外的缩放开销
    // 画布渲染时会处理缩放
    int output_width = width_;
    int output_height = height_;

    auto videoFrame = std::make_shared<VideoFrame>(output_width, output_height);
    videoFrame->format = VideoFrame::PixelFormat::RGBA;

    uint8_t* dstData[4] = { videoFrame->data.get(), nullptr, nullptr, nullptr };
    int dstLinesize[4] = { output_width * 4, 0, 0, 0 };

    // 使用时间戳来测量格式转换性能
    auto scale_start = std::chrono::high_resolution_clock::now();

    sws_scale(sws_ctx_,
        frame->data, frame->linesize,
        0, frame->height,
        dstData, dstLinesize);

    auto scale_end = std::chrono::high_resolution_clock::now();
    auto scale_duration = std::chrono::duration_cast<std::chrono::microseconds>(scale_end - scale_start).count();

    // 每60帧记录一次格式转换性能
    static int frame_counter = 0;
    static int64_t total_scale_time_us = 0;
    frame_counter++;
    total_scale_time_us += scale_duration;

    if (frame_counter % 60 == 0) {
        LOG_INFO("[MediaFileSource] Scale perf: avg=" + std::to_string(total_scale_time_us / 60) +
                 "us, last=" + std::to_string(scale_duration) + "us, " +
                 std::to_string(width_) + "x" + std::to_string(height_) + " -> RGBA");
        total_scale_time_us = 0;
    }

    videoFrame->stride = output_width * 4;
    videoFrame->timestamp_ms = frame->pts != AV_NOPTS_VALUE ?
        frame->pts * av_q2d(format_ctx_->streams[video_stream_idx_]->time_base) * 1000 : 0;

    return videoFrame;
}

std::shared_ptr<AudioFrame> MediaFileSource::convertToAudioFrame(AVFrame* frame) {
    if (!frame || !swr_ctx_) return nullptr;

    // 计算时间戳
    int64_t frame_timestamp_ms = frame->pts != AV_NOPTS_VALUE ?
        frame->pts * av_q2d(format_ctx_->streams[audio_stream_idx_]->time_base) * 1000 : 0;

    // 执行重采样
    int outSamples = swr_get_out_samples(swr_ctx_, frame->nb_samples);
    
    // 创建临时缓冲区
    std::vector<float> temp_buffer(outSamples * target_channels_);
    
    uint8_t* outData = reinterpret_cast<uint8_t*>(temp_buffer.data());
    int ret = swr_convert(swr_ctx_, &outData, outSamples,
        (const uint8_t**)frame->data, frame->nb_samples);

    if (ret < 0) {
        return nullptr;
    }

    // 将重采样数据添加到缓冲区
    int actual_samples = ret;
    size_t buffer_size_before = audio_resample_buffer_.size();
    audio_resample_buffer_.insert(audio_resample_buffer_.end(), 
                                   temp_buffer.begin(), 
                                   temp_buffer.begin() + actual_samples * target_channels_);
    
    // 记录第一个包的时间戳
    if (audio_resample_timestamp_ms_ == 0 && frame_timestamp_ms > 0) {
        audio_resample_timestamp_ms_ = frame_timestamp_ms;
    }

    // 检查缓冲区是否足够1024样本
    // 只有当新添加的数据后才能创建新帧
    size_t buffer_size_after = audio_resample_buffer_.size();
    bool has_new_data = (buffer_size_after > buffer_size_before);
    
    if (has_new_data && audio_resample_buffer_.size() >= TARGET_AUDIO_SAMPLES * target_channels_) {
        // 创建固定1024样本的输出帧
        auto audioFrame = std::make_shared<AudioFrame>(target_sample_rate_, target_channels_, TARGET_AUDIO_SAMPLES);
        
        // 复制数据
        memcpy(audioFrame->data, audio_resample_buffer_.data(), 
               TARGET_AUDIO_SAMPLES * target_channels_ * sizeof(float));
        
        // 设置时间戳（基于缓冲区累积）
        audioFrame->timestamp_ms = audio_resample_timestamp_ms_;
        audio_resample_timestamp_ms_ += (TARGET_AUDIO_SAMPLES * 1000LL / target_sample_rate_);

        // 移除已使用的数据
        audio_resample_buffer_.erase(audio_resample_buffer_.begin(), 
                                      audio_resample_buffer_.begin() + TARGET_AUDIO_SAMPLES * target_channels_);

        // 将帧放入队列
        std::lock_guard<std::mutex> lock(audio_frame_mutex_);
        if (audio_frame_queue_.size() < MAX_FRAME_QUEUE_SIZE) {
            audio_frame_queue_.push(audioFrame);
        }
        
        return audioFrame;
    }
    
    return nullptr;  // 缓冲区不够1024样本或没有新数据
}

}  // namespace live_assistant
