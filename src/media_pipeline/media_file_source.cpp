#include "media_pipeline/media_file_source.h"
#include "video_engine/video_engine.h"
#include "audio_engine/audio_engine.h"
#include "common/log.h"
#include <QFile>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
}

// VideoFrame 和 AudioFrame 已在 video_engine.h 和 audio_engine.h 中定义

namespace live_assistant {

MediaFileSource::MediaFileSource(const std::string& id, std::shared_ptr<InsertFileItem> file_item)
    : Source(id, Type::FILE_SOURCE)
    , file_item_(file_item) {
}

MediaFileSource::~MediaFileSource() {
    shutdown();
}

bool MediaFileSource::initialize() {
    if (!file_item_) {
        LOG_ERROR("MediaFileSource: No file item provided");
        return false;
    }

    QString localPath = file_item_->getLocalCachePath();
    if (!QFile::exists(localPath)) {
        LOG_ERROR("MediaFileSource: File not found: " + localPath.toStdString());
        return false;
    }

    if (!initializeReader()) {
        return false;
    }

    if (!initializeDecoder()) {
        shutdownReader();
        return false;
    }

    LOG_INFO("MediaFileSource: Initialized successfully for " + file_item_->fileName.toStdString());
    return true;
}

bool MediaFileSource::start() {
    if (running_.exchange(true)) {
        return true; // 已经在运行
    }

    reader_finished_ = false;
    finished_ = false;
    start_time_ = std::chrono::steady_clock::now();

    // 启动读取线程
    reader_thread_ = std::thread(&MediaFileSource::readerThreadFunc, this);

    // 启动调度线程
    scheduler_thread_ = std::thread(&MediaFileSource::schedulerThreadFunc, this);

    LOG_INFO("MediaFileSource: Started playback");
    return true;
}

bool MediaFileSource::stop() {
    if (!running_.exchange(false)) {
        return true; // 已经停止
    }

    // 通知所有等待的线程
    video_packet_cv_.notify_all();
    audio_packet_cv_.notify_all();
    video_frame_cv_.notify_all();
    audio_frame_cv_.notify_all();

    // 等待线程结束
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
    if (scheduler_thread_.joinable()) {
        scheduler_thread_.join();
    }

    LOG_INFO("MediaFileSource: Stopped playback");
    return true;
}

bool MediaFileSource::shutdown() {
    stop();
    shutdownDecoder();
    shutdownReader();
    return true;
}

std::shared_ptr<AudioFrame> MediaFileSource::get_audio_frame() {
    std::unique_lock<std::mutex> lock(audio_frame_mutex_);
    if (audio_frame_queue_.empty()) {
        return nullptr;
    }
    auto frame = audio_frame_queue_.front();
    audio_frame_queue_.pop();
    return frame;
}

std::shared_ptr<VideoFrame> MediaFileSource::get_video_frame() {
    std::unique_lock<std::mutex> lock(video_frame_mutex_);
    if (video_frame_queue_.empty()) {
        return nullptr;
    }
    auto frame = video_frame_queue_.front();
    video_frame_queue_.pop();
    return frame;
}

void MediaFileSource::seek(int64_t position_ms) {
    seek_position_ms_ = position_ms;
    seek_requested_.store(true);
}

// ========== Reader 逻辑 ==========

bool MediaFileSource::initializeReader() {
    QString localPath = file_item_->getLocalCachePath();

    // 打开输入文件
    if (avformat_open_input(&format_ctx_, localPath.toUtf8().constData(), nullptr, nullptr) < 0) {
        LOG_ERROR("MediaFileSource: Failed to open input file");
        return false;
    }

    // 获取流信息
    if (avformat_find_stream_info(format_ctx_, nullptr) < 0) {
        LOG_ERROR("MediaFileSource: Failed to find stream info");
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

    LOG_INFO("MediaFileSource: Video stream idx=" + std::to_string(video_stream_idx_) +
             ", Audio stream idx=" + std::to_string(audio_stream_idx_));
    LOG_INFO("MediaFileSource: Resolution=" + std::to_string(width_) + "x" + std::to_string(height_) +
             ", FPS=" + std::to_string(video_fps_) +
             ", Duration=" + std::to_string(duration_ms_) + "ms");

    return true;
}

void MediaFileSource::readerThreadFunc() {
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        LOG_ERROR("MediaFileSource: Failed to allocate packet");
        return;
    }

    while (running_.load()) {
        // 处理 seek 请求
        if (seek_requested_.exchange(false)) {
            int64_t seek_target = seek_position_ms_ * AV_TIME_BASE / 1000;
            av_seek_frame(format_ctx_, -1, seek_target, AVSEEK_FLAG_BACKWARD);
            // 清空队列
            {
                std::lock_guard<std::mutex> lock(video_packet_mutex_);
                std::queue<MediaPacket> empty;
                std::swap(video_packet_queue_, empty);
            }
            {
                std::lock_guard<std::mutex> lock(audio_packet_mutex_);
                std::queue<MediaPacket> empty;
                std::swap(audio_packet_queue_, empty);
            }
        }

        // 检查队列是否已满
        {
            std::lock_guard<std::mutex> lock(video_packet_mutex_);
            if (video_packet_queue_.size() >= MAX_QUEUE_SIZE) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
        }

        int ret = av_read_frame(format_ctx_, packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                LOG_INFO("MediaFileSource: End of file reached");
                reader_finished_ = true;
            } else {
                LOG_ERROR("MediaFileSource: Error reading frame");
            }
            break;
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
}

void MediaFileSource::shutdownReader() {
    if (format_ctx_) {
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
    }
}

// ========== Scheduler 逻辑 ==========

bool MediaFileSource::initializeDecoder() {
    // 初始化视频解码器
    if (video_stream_idx_ >= 0) {
        AVStream* stream = format_ctx_->streams[video_stream_idx_];
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) {
            LOG_ERROR("MediaFileSource: Failed to find video codec");
            return false;
        }

        video_codec_ctx_ = avcodec_alloc_context3(codec);
        if (!video_codec_ctx_) {
            LOG_ERROR("MediaFileSource: Failed to allocate video codec context");
            return false;
        }

        if (avcodec_parameters_to_context(video_codec_ctx_, stream->codecpar) < 0) {
            LOG_ERROR("MediaFileSource: Failed to copy video codec params");
            avcodec_free_context(&video_codec_ctx_);
            return false;
        }

        if (avcodec_open2(video_codec_ctx_, codec, nullptr) < 0) {
            LOG_ERROR("MediaFileSource: Failed to open video codec");
            avcodec_free_context(&video_codec_ctx_);
            return false;
        }

        // 分配视频帧
        video_frame_ = av_frame_alloc();
        if (!video_frame_) {
            LOG_ERROR("MediaFileSource: Failed to allocate video frame");
            return false;
        }

        // 初始化视频转换上下文 (YUV -> RGBA)
        sws_ctx_ = sws_getContext(
            width_, height_, video_codec_ctx_->pix_fmt,
            target_width_, target_height_, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
    }

    // 初始化音频解码器
    if (audio_stream_idx_ >= 0) {
        AVStream* stream = format_ctx_->streams[audio_stream_idx_];
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) {
            LOG_ERROR("MediaFileSource: Failed to find audio codec");
            return false;
        }

        audio_codec_ctx_ = avcodec_alloc_context3(codec);
        if (!audio_codec_ctx_) {
            LOG_ERROR("MediaFileSource: Failed to allocate audio codec context");
            return false;
        }

        if (avcodec_parameters_to_context(audio_codec_ctx_, stream->codecpar) < 0) {
            LOG_ERROR("MediaFileSource: Failed to copy audio codec params");
            avcodec_free_context(&audio_codec_ctx_);
            return false;
        }

        if (avcodec_open2(audio_codec_ctx_, codec, nullptr) < 0) {
            LOG_ERROR("MediaFileSource: Failed to open audio codec");
            avcodec_free_context(&audio_codec_ctx_);
            return false;
        }

        // 分配音频帧
        audio_frame_ = av_frame_alloc();
        if (!audio_frame_) {
            LOG_ERROR("MediaFileSource: Failed to allocate audio frame");
            return false;
        }

        // 初始化音频重采样上下文
        AVChannelLayout out_ch_layout;
        av_channel_layout_default(&out_ch_layout, target_channels_);

        swr_alloc_set_opts2(&swr_ctx_,
            &out_ch_layout, AV_SAMPLE_FMT_S16, target_sample_rate_,
            &audio_codec_ctx_->ch_layout, audio_codec_ctx_->sample_fmt, audio_codec_ctx_->sample_rate,
            0, nullptr);

        if (swr_ctx_ && swr_init(swr_ctx_) < 0) {
            LOG_ERROR("MediaFileSource: Failed to initialize audio resampler");
            swr_free(&swr_ctx_);
        }
    }

    return true;
}

void MediaFileSource::schedulerThreadFunc() {
    auto frame_interval = std::chrono::milliseconds(1000 / target_fps_);
    auto next_frame_time = std::chrono::steady_clock::now();

    while (running_.load()) {
        if (paused_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            next_frame_time = std::chrono::steady_clock::now() + frame_interval;
            continue;
        }

        // 处理视频帧
        if (video_stream_idx_ >= 0) {
            std::unique_lock<std::mutex> lock(video_packet_mutex_);
            if (video_packet_queue_.empty()) {
                if (reader_finished_.load()) {
                    // 读取完成且队列为空，播放结束
                    break;
                }
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            MediaPacket packet = std::move(video_packet_queue_.front());
            video_packet_queue_.pop();
            lock.unlock();

            // 解码视频包
            AVPacket* avPacket = av_packet_alloc();
            avPacket->data = reinterpret_cast<uint8_t*>(packet.data.data());
            avPacket->size = packet.data.size();
            avPacket->pts = packet.pts;
            avPacket->dts = packet.pts;

            if (decodeVideoPacket(avPacket)) {
                // 按帧率调度
                auto now = std::chrono::steady_clock::now();
                if (now >= next_frame_time) {
                    auto videoFrame = convertToVideoFrame(video_frame_);
                    if (videoFrame) {
                        {
                            std::lock_guard<std::mutex> frameLock(video_frame_mutex_);
                            if (video_frame_queue_.size() < MAX_FRAME_QUEUE_SIZE) {
                                video_frame_queue_.push(videoFrame);
                                video_frame_cv_.notify_one();
                            }
                        }

                        if (frame_ready_callback_) {
                            frame_ready_callback_(videoFrame);
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
                    next_frame_time += frame_interval;
                }
            }

            av_packet_free(&avPacket);
        }

        // 处理音频帧
        if (audio_stream_idx_ >= 0) {
            std::unique_lock<std::mutex> lock(audio_packet_mutex_);
            if (!audio_packet_queue_.empty()) {
                MediaPacket packet = std::move(audio_packet_queue_.front());
                audio_packet_queue_.pop();
                lock.unlock();

                AVPacket* avPacket = av_packet_alloc();
                avPacket->data = reinterpret_cast<uint8_t*>(packet.data.data());
                avPacket->size = packet.data.size();
                avPacket->pts = packet.pts;
                avPacket->dts = packet.pts;

                if (decodeAudioPacket(avPacket)) {
                    auto audioFrame = convertToAudioFrame(audio_frame_);
                    if (audioFrame) {
                        {
                            std::lock_guard<std::mutex> frameLock(audio_frame_mutex_);
                            if (audio_frame_queue_.size() < MAX_FRAME_QUEUE_SIZE) {
                                audio_frame_queue_.push(audioFrame);
                                audio_frame_cv_.notify_one();
                            }
                        }

                        if (audio_ready_callback_) {
                            audio_ready_callback_(audioFrame);
                        }
                    }
                }

                av_packet_free(&avPacket);
            } else {
                lock.unlock();
            }
        }

        // 控制循环频率
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    finished_ = true;
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
}

// ========== 解码辅助函数 ==========

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

    auto videoFrame = std::make_shared<VideoFrame>(target_width_, target_height_);
    videoFrame->format = VideoFrame::PixelFormat::RGBA;

    // 使用 sws_scale 进行格式转换
    uint8_t* dstData[4] = { videoFrame->data.get(), nullptr, nullptr, nullptr };
    int dstLinesize[4] = { target_width_ * 4, 0, 0, 0 };

    sws_scale(sws_ctx_,
        frame->data, frame->linesize,
        0, frame->height,
        dstData, dstLinesize);

    videoFrame->stride = target_width_ * 4;
    videoFrame->timestamp_ms = frame->pts != AV_NOPTS_VALUE ?
        frame->pts * av_q2d(format_ctx_->streams[video_stream_idx_]->time_base) * 1000 : 0;

    return videoFrame;
}

std::shared_ptr<AudioFrame> MediaFileSource::convertToAudioFrame(AVFrame* frame) {
    if (!frame || !swr_ctx_) return nullptr;

    // 计算输出样本数
    int outSamples = swr_get_out_samples(swr_ctx_, frame->nb_samples);

    auto audioFrame = std::make_shared<AudioFrame>(target_sample_rate_, target_channels_, outSamples);

    // 重采样
    uint8_t* outData = reinterpret_cast<uint8_t*>(audioFrame->raw_data);
    int ret = swr_convert(swr_ctx_, &outData, outSamples,
        (const uint8_t**)frame->data, frame->nb_samples);

    if (ret < 0) {
        return nullptr;
    }

    audioFrame->timestamp_ms = frame->pts != AV_NOPTS_VALUE ?
        frame->pts * av_q2d(format_ctx_->streams[audio_stream_idx_]->time_base) * 1000 : 0;

    return audioFrame;
}

}  // namespace live_assistant
