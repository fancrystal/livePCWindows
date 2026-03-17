#include "scene_manager/compositor_encoder_bridge.h"
#include "common/log.h"
#include "common/error.h"
#include "video_engine/video_engine.h"
#include "audio_engine/audio_engine.h"
#include "encoder/audio_encoder.h"
#include "media_pipeline/media_file_source.h"

#include <QImage>
#include <QPainter>
#include <cstring>
#include <chrono>

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

namespace live_assistant {

CompositorEncoderBridge::CompositorEncoderBridge(QObject* parent)
    : QObject(parent), media_clock_() {
    // 使用工作线程定时器替代 QTimer（避免阻塞主线程）
    // 定时器在工作线程中运行，不会阻塞 Qt 主线程
    last_capture_time_ = std::chrono::steady_clock::now();
    
    // 捕获 this 指针，避免 lambda 捕获问题
    auto* bridge = this;
    capture_thread_ = std::make_unique<std::thread>([bridge]() {
        while (!bridge->capture_thread_stop_) {
            auto thread_now = std::chrono::steady_clock::now();
            int current_fps = bridge->capture_fps_.load();
            int interval_ms = (current_fps > 0) ? (1000 / current_fps) : 33;
            auto next_tick = bridge->last_capture_time_ + std::chrono::milliseconds(interval_ms);
            
            if (thread_now < next_tick) {
                std::this_thread::sleep_for(next_tick - thread_now);
            }
            
            if (!bridge->capture_thread_stop_ && bridge->running_) {
                bridge->last_capture_time_ = std::chrono::steady_clock::now();
                // 使用 QueuedConnection 确保在主线程执行
                QMetaObject::invokeMethod(bridge, "on_encode_timer", Qt::QueuedConnection);
            } else {
                // 如果没有运行，稍作等待避免忙轮询
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    });

    // 初始化插播视频帧同步器（8帧缓冲，约267ms @ 30fps）
    insert_video_synchronizer_ = std::make_unique<VideoFrameSynchronizer>();
    insert_video_synchronizer_->set_max_size(8);

    LOG_INFO("CompositorEncoderBridge created with worker thread timer");
}

CompositorEncoderBridge::~CompositorEncoderBridge() {
    // 停止工作线程定时器
    capture_thread_stop_.store(true);
    if (capture_thread_ && capture_thread_->joinable()) {
        capture_thread_->join();
    }
    
    stop();
    release_sws_context();
    LOG_INFO("CompositorEncoderBridge destroyed");
}

void CompositorEncoderBridge::enable_adaptive_quality(bool enable) {
    adaptive_quality_enabled_ = enable;
    LOG_INFO("Adaptive quality " + std::string(enable ? "enabled" : "disabled"));
}

void CompositorEncoderBridge::set_quality_thresholds(int min_fps, int max_fps) {
    min_fps_threshold_ = min_fps;
    max_fps_threshold_ = max_fps;
    LOG_INFO("Quality thresholds set: min=" + std::to_string(min_fps) + ", max=" + std::to_string(max_fps));
}

void CompositorEncoderBridge::set_compositor(std::shared_ptr<Compositor> compositor) {
    compositor_ = compositor;
    if (compositor_) {
        connect(compositor_.get(), &Compositor::frame_ready,
                this, &CompositorEncoderBridge::on_compositor_frame_ready);
        LOG_INFO("Compositor set for encoder bridge");
    }
}

void CompositorEncoderBridge::set_canvas_renderer(std::shared_ptr<CanvasRenderer> renderer, std::shared_ptr<Scene> scene) {
    canvas_renderer_ = renderer;
    current_scene_ = scene;
    LOG_INFO("CanvasRenderer set for encoder bridge (fallback mode)");
}

void CompositorEncoderBridge::set_encoder(std::shared_ptr<Encoder> encoder) {
    if (encoder_ && encoder_->get_audio_encoder()) {
        auto* old_audio_encoder = dynamic_cast<AACEncoder*>(encoder_->get_audio_encoder());
        if (old_audio_encoder) {
            disconnect(old_audio_encoder, &AACEncoder::audio_encoded,
                       this, &CompositorEncoderBridge::on_audio_encoded);
        }
    }

    encoder_ = encoder;

    if (encoder_ && encoder_->get_audio_encoder()) {
        auto* audio_encoder = dynamic_cast<AACEncoder*>(encoder_->get_audio_encoder());
        if (audio_encoder) {
            connect(audio_encoder, &AACEncoder::audio_encoded,
                    this, &CompositorEncoderBridge::on_audio_encoded);
            LOG_INFO("Encoder set for encoder bridge and audio encoder signal connected");
        } else {
            LOG_INFO("Encoder set for encoder bridge (audio encoder not AACEncoder)");
        }
    } else {
        LOG_INFO("Encoder set for encoder bridge (null or no audio encoder)");
    }
}

void CompositorEncoderBridge::set_stream_pusher(std::shared_ptr<StreamPusher> stream_pusher) {
    stream_pusher_ = stream_pusher;
    LOG_INFO("Stream pusher set for encoder bridge");
}

void CompositorEncoderBridge::set_audio_engine(std::shared_ptr<AudioEngine> audio_engine) {
    if (audio_engine_) {
        disconnect(audio_engine_.get(), &AudioEngine::audio_data_ready,
                   this, &CompositorEncoderBridge::on_audio_data_ready);
    }

    audio_engine_ = audio_engine;

    if (audio_engine_) {
        connect(audio_engine_.get(), &AudioEngine::audio_data_ready,
                this, &CompositorEncoderBridge::on_audio_data_ready);
        LOG_INFO("Audio engine set for encoder bridge and signal connected");
    } else {
        LOG_INFO("Audio engine set for encoder bridge (null)");
    }
}

void CompositorEncoderBridge::set_silent_audio(bool enable) {
    silent_audio_enabled_ = enable;
    LOG_INFO(std::string("Silent audio ") + (enable ? "enabled" : "disabled"));
}

void CompositorEncoderBridge::start(int fps) {
    if (running_) {
        LOG_WARNING("Encoder bridge already running");
        return;
    }

    if (!compositor_ || !encoder_) {
        LOG_ERROR("Compositor or encoder not set");
        return;
    }

    fps_ = fps;
    running_ = true;
    video_frame_count_ = 0;
    capture_fps_.store(fps);
    last_capture_time_ = std::chrono::steady_clock::now();

    media_clock_.start();

    initialize_opengl_context();

    // 不再需要启动 QTimer，工作线程已经在运行

    start_encoder_threads();

    LOG_INFO("CompositorEncoderBridge started with " + std::to_string(fps) + " fps, media_clock started");
}

void CompositorEncoderBridge::stop() {
    if (!running_) {
        return;
    }

    running_ = false;
    // 不再需要停止 QTimer

    stop_encoder_threads();

    media_clock_.stop();

    LOG_INFO("CompositorEncoderBridge stopped");
}

bool CompositorEncoderBridge::is_running() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return running_;
}

bool CompositorEncoderBridge::start_streaming(const std::string& url) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (streaming_) {
        LOG_WARNING("Already streaming");
        return true;
    }

    if (!stream_pusher_) {
        LOG_ERROR("Stream pusher not set");
        emit streaming_error("Stream pusher not initialized");
        return false;
    }

    // 设置推流配置
    StreamConfig config;

    // 解析URL，分离服务器地址和流密钥
    std::string server_url = url;
    std::string stream_key;

    // 简单解析 rtmp://server/app/stream -> server_url: rtmp://server/app, stream_key: stream
    auto trim = [](std::string s) {
        const char* ws = " \t\n\r\f\v";
        size_t start = s.find_first_not_of(ws);
        if (start == std::string::npos) return std::string();
        size_t end = s.find_last_not_of(ws);
        return s.substr(start, end - start + 1);
    };
    std::string url_trim = trim(url);
    size_t last_slash = url_trim.find_last_of('/');
    if (last_slash != std::string::npos && last_slash > 7) {
        server_url = url_trim.substr(0, last_slash);
        stream_key = url_trim.substr(last_slash + 1);
    }

    config.server_url = server_url;
    config.stream_key = stream_key;
    config.protocol = StreamProtocol::RTMP;

    ErrorCode config_result = stream_pusher_->set_config(config);
    if (config_result != ErrorCode::SUCCESS) {
        LOG_ERROR("Failed to set stream config for: " + url);
        emit streaming_error(QString("Failed to set stream config: %1").arg(static_cast<int>(config_result)));
        return false;
    }

    // Ensure audio/video streams are registered before starting the pusher
    if (encoder_) {
        AVCodecParameters* a_par = encoder_->get_audio_codec_parameters();
        AVRational a_tb = encoder_->get_audio_time_base();
        AVCodecParameters* v_par = encoder_->get_video_codec_parameters();
        AVRational v_tb = encoder_->get_video_time_base();

        // If video codec params are null, attempt to fallback to software encoder
        if (!v_par && encoder_) {
            LOG_WARNING("[BRIDGE] Video codec parameters null; attempting encoder fallback to software");
            VideoEncoderConfig vc = encoder_->get_video_config();
            vc.prefer_hw = false;
            vc.hw_accel = HWAccelerationType::NONE;
            ErrorCode r = encoder_->reinitialize_video_encoder(vc);
            if (r == ErrorCode::SUCCESS) {
                v_par = encoder_->get_video_codec_parameters();
                v_tb = encoder_->get_video_time_base();
            }
        }

        if (!a_par || !v_par || a_tb.den <= 0 || v_tb.den <= 0) {
            if (a_par) avcodec_parameters_free(&a_par);
            if (v_par) avcodec_parameters_free(&v_par);
            LOG_ERROR("[BRIDGE] Audio/Video codec parameters unavailable or invalid, cannot start streaming");
            emit streaming_error(QString("Failed to start streaming: %1").arg(static_cast<int>(ErrorCode::INVALID_STATE)));
            return false;
        }

        ErrorCode ra = stream_pusher_->register_audio_stream(a_par, a_tb);
        ErrorCode rv = stream_pusher_->register_video_stream(v_par, v_tb);

        avcodec_parameters_free(&a_par);
        avcodec_parameters_free(&v_par);

        if (ra != ErrorCode::SUCCESS || rv != ErrorCode::SUCCESS) {
            LOG_ERROR("[BRIDGE] Failed to register audio/video streams");
            emit streaming_error(QString("Failed to start streaming: %1").arg(static_cast<int>(ErrorCode::INVALID_STATE)));
            return false;
        }
    }

    running_ = false;
    // 不再需要停止 QTimer

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (encoder_) {
        encoder_->reset_audio_encoder();
        // 注意：不在这里重置视频编码器，因为 reset() 可能导致编码器状态异常
        // 视频编码器应该在停止推流时清理，而不是在开始时重置
    }

    if (stream_pusher_) {
        stream_pusher_->clear_queue();
    }

    {
        std::lock_guard<std::mutex> lock(encode_queue_mutex_);
        int cleared_frames = static_cast<int>(pre_encode_queue_.size());
        pre_encode_queue_.clear();
        if (cleared_frames > 0) {
            LOG_INFO("[BRIDGE] Cleared " + std::to_string(cleared_frames) + " frames from encode queue");
        }
    }

    video_frame_count_ = 0;
    
    // 🔧 OBS 风格 PTS 偏移归零：重置偏移变量，等待第一帧到达时重新记录
    first_video_pts_ms_ = -1;
    streaming_pts_initialized_ = false;
    LOG_INFO("[BRIDGE] PTS offset reset, waiting for first frame");

    media_clock_.start();
    LOG_INFO("[BRIDGE] Media clock started");

    ErrorCode start_result = stream_pusher_->start();
    if (start_result != ErrorCode::SUCCESS) {
        LOG_ERROR("Failed to start streaming to: " + url);
        emit streaming_error(QString("Failed to start streaming: %1").arg(static_cast<int>(start_result)));
        media_clock_.stop();
        return false;
    }

    running_ = true;
    capture_fps_.store(fps_);
    last_capture_time_ = std::chrono::steady_clock::now();
    // 不再需要启动 QTimer，工作线程已经在运行

    LOG_INFO("[BRIDGE] Encoding restarted, media_clock: " + std::to_string(media_clock_.get_elapsed_time_us() / 1000) + "ms");

    streaming_ = true;
    stream_url_ = url;
    emit streaming_started();
    LOG_INFO("Streaming started to: " + url);
    return true;
}

void CompositorEncoderBridge::stop_streaming() {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (!streaming_) {
        return;
    }

    // ═══════════════════════════════════════════════════════════════
    // 🔧 停止编码线程池
    // ═══════════════════════════════════════════════════════════════
    stop_encoder_threads();

    if (stream_pusher_) {
        stream_pusher_->stop();
    }

    // 停止媒体时钟
    media_clock_.stop();

    streaming_ = false;
    stream_url_.clear();
    emit streaming_stopped();
    LOG_INFO("[BRIDGE] Streaming stopped");
}

bool CompositorEncoderBridge::is_streaming() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return streaming_;
}

void CompositorEncoderBridge::set_fps(int fps) {
    fps_ = fps;
    capture_fps_.store(fps);
    if (running_) {
        LOG_INFO("FPS changed to " + std::to_string(fps));
    }
}

void CompositorEncoderBridge::set_resolution(int width, int height) {
    width_ = width;
    height_ = height;
    if (compositor_) {
        compositor_->set_canvas_size(width, height);
    }
    LOG_INFO("Resolution set to " + std::to_string(width) + "x" + std::to_string(height));
}

void CompositorEncoderBridge::on_compositor_frame_ready() {
    // timer-based
}

void CompositorEncoderBridge::on_encode_timer() {
    if (!running_) {
        return;
    }

    if (adaptive_quality_enabled_ && compositor_) {
        double avg_fps, avg_render_time;
        size_t frame_count;
        compositor_->get_performance_stats(avg_fps, avg_render_time, frame_count);

        if (!quality_degraded_ && avg_fps < min_fps_threshold_ && avg_fps > 0) {
            // Quality degradation: reduce FPS
            original_fps_ = fps_;
            fps_ = degraded_fps_;
            capture_fps_.store(fps_);
            quality_degraded_ = true;

            emit quality_degraded(QString("FPS dropped to %1, reducing to %2 fps").arg(avg_fps).arg(fps_));
            LOG_WARNING("Quality degraded due to low FPS: " + std::to_string(avg_fps));

        } else if (quality_degraded_) {
            // Recovery condition: FPS has recovered to at least 80% of original FPS
            // This ensures recovery is achievable while preventing oscillation
            double recovery_threshold = original_fps_ * 0.8;
            if (avg_fps > recovery_threshold && avg_fps >= degraded_fps_) {
                fps_ = original_fps_;
                capture_fps_.store(fps_);
                quality_degraded_ = false;

                emit quality_restored();
                LOG_INFO("Quality restored, FPS recovered to: " + std::to_string(avg_fps) +
                         " (threshold was: " + std::to_string(recovery_threshold) + ")");
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════
    // 🔧 异步编码架构：只捕获帧，加入编码队列
    // 编码在独立线程中进行，不阻塞定时器
    // ═══════════════════════════════════════════════════════════════
    capture_and_queue_frame();
}

void CompositorEncoderBridge::capture_and_queue_frame() {
    try {
        int64_t frame_duration_ms = 1000 / fps_;

        // [DIAG] Frame capture start time
        auto frame_start = std::chrono::high_resolution_clock::now();
        
        video_frame_count_++;
        
        auto current_pts_ms = media_clock_.get_elapsed_time_us() / 1000;
        
        // 🔧 OBS 风格 PTS 偏移归零：记录第一帧视频 PTS 并应用偏移
        // 注意：需要同时检查 video 和 audio 的 first_pts，确保任一帧到达时都能正确初始化
        if (first_video_pts_ms_ == -1) {
            first_video_pts_ms_ = current_pts_ms;
            LOG_INFO("[BRIDGE] First video PTS recorded: " + std::to_string(first_video_pts_ms_) + "ms");
        }
        
        // 如果 streaming_pts_initialized_ 还未初始化（音频未到达），现在初始化
        if (!streaming_pts_initialized_) {
            streaming_pts_initialized_ = true;
            LOG_INFO("[BRIDGE] Streaming PTS initialized by video at: " + std::to_string(current_pts_ms) + "ms");
        }
        
        // 应用 PTS 偏移，确保第一帧视频 PTS=0
        int64_t adjusted_pts_ms = current_pts_ms - first_video_pts_ms_;
        
        if (video_frame_count_ <= 3 || video_frame_count_ % 100 == 0) {
            LOG_INFO("[BRIDGE] Video frame #" + std::to_string(video_frame_count_) + 
                     ": raw_pts=" + std::to_string(current_pts_ms) + "ms, " +
                     "adjusted_pts=" + std::to_string(adjusted_pts_ms) + "ms, " +
                     "offset=" + std::to_string(first_video_pts_ms_) + "ms");
        }

    auto video_frame = capture_compositor_frame();

    // [DIAG] Capture total time (render + SWS)
    auto capture_end = std::chrono::high_resolution_clock::now();
    auto capture_total_us = std::chrono::duration_cast<std::chrono::microseconds>(capture_end - frame_start).count();
    if (video_frame_count_ <= 3 || video_frame_count_ % 100 == 0) {
        LOG_DEBUG("[BRIDGE] Frame #" + std::to_string(video_frame_count_) + 
                 " capture_total=" + std::to_string(capture_total_us) + "us");
    }
    
    if (video_frame) {
            video_frame->timestamp_ms = adjusted_pts_ms;

            PreEncodeVideoFrame pre_frame;
            pre_frame.frame = video_frame;
            pre_frame.pts_ms = adjusted_pts_ms;
            pre_frame.wallclock_us = adjusted_pts_ms * 1000;
            pre_frame.frame_count = video_frame_count_;
            
            push_to_encode_queue(pre_frame);

            // [DIAG] Frame capture total time
            auto frame_end = std::chrono::high_resolution_clock::now();
            auto frame_capture_us = std::chrono::duration_cast<std::chrono::microseconds>(frame_end - frame_start).count();
            if (video_frame_count_ <= 3 || video_frame_count_ % 100 == 0) {
                LOG_DEBUG("[BRIDGE] Frame #" + std::to_string(video_frame_count_) + 
                         " capture_time=" + std::to_string(frame_capture_us) + "us");
            }
        }

    } catch (const std::exception& ex) {
        LOG_ERROR("[BRIDGE] Exception in capture_and_queue_frame: " + std::string(ex.what()));
    }
}

std::shared_ptr<VideoFrame> CompositorEncoderBridge::capture_compositor_frame() {
    // ═══════════════════════════════════════════════════════════════════
    // 优先使用 Compositor（如果所有源都正确更新了帧到 Compositor）
    // ═══════════════════════════════════════════════════════════════════
    if (compositor_) {
        // 直接获取 RGBA8888 格式的图像（render_to_image 已优化为 RGBA8888）
        const QImage img = compositor_->render_to_image(width_, height_);
        if (!img.isNull()) {
            return convert_qimage_to_video_frame(img);
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // 回退方案：使用 CanvasRenderer（能正确处理所有类型的源）
    // ═══════════════════════════════════════════════════════════════════
    if (canvas_renderer_ && current_scene_) {
        QImage img(width_, height_, QImage::Format_RGBA8888);
        img.fill(Qt::black);
        
        QPainter painter(&img);
        QRect target_rect(0, 0, width_, height_);
        canvas_renderer_->render(painter, current_scene_, target_rect, nullptr);
        painter.end();
        
        if (!img.isNull()) {
            return convert_qimage_to_video_frame(img);
        }
    }

    return nullptr;
}

// 辅助函数：QImage 转 VideoFrame
std::shared_ptr<VideoFrame> CompositorEncoderBridge::convert_qimage_to_video_frame(const QImage& img) {
    if (img.isNull()) {
        return nullptr;
    }

    // Convert composed RGBA image to NV12 for (future) HW-friendly pipeline.
    auto frame = std::make_shared<VideoFrame>();
    frame->format = VideoFrame::PixelFormat::NV12;
    frame->width = width_;
    frame->height = height_;
    frame->stride = width_;
    frame->stride_uv = width_;

    const int y_size = frame->stride * frame->height;
    const int uv_size = frame->stride_uv * (frame->height / 2);
    frame->data = std::make_unique<uint8_t[]>(y_size);
    frame->data_uv = std::make_unique<uint8_t[]>(uv_size);
    if (!frame->data || !frame->data_uv) {
        return nullptr;
    }

    // Get or create cached SWS context (reuses context when resolution unchanged)
    SwsContext* sws = static_cast<SwsContext*>(get_or_create_sws_context(width_, height_));
    if (!sws) {
        LOG_ERROR("Failed to get or create SWS context for RGBA to NV12 conversion");
        return nullptr;
    }

    const uint8_t* src_slices[1] = { reinterpret_cast<const uint8_t*>(img.constBits()) };
    int src_strides[1] = { static_cast<int>(img.bytesPerLine()) };

    uint8_t* dst_slices[2] = { frame->data.get(), frame->data_uv.get() };
    int dst_strides[2] = { frame->stride, frame->stride_uv };

    // [DIAG] SWS conversion performance
    auto sws_start = std::chrono::high_resolution_clock::now();
    sws_scale(sws, src_slices, src_strides, 0, height_, dst_slices, dst_strides);
    auto sws_end = std::chrono::high_resolution_clock::now();
    auto sws_us = std::chrono::duration_cast<std::chrono::microseconds>(sws_end - sws_start).count();
    LOG_DEBUG("[BRIDGE] SWS RGBA->NV12: " + std::to_string(sws_us) + "us, " +
              std::to_string(width_) + "x" + std::to_string(height_));
    // Note: sws_freeContext() is no longer called here - context is cached and released in destructor

    return frame;
}

void CompositorEncoderBridge::initialize_opengl_context() {
    LOG_INFO("OpenGL context initialization skipped for encoder bridge");
}

void* CompositorEncoderBridge::get_or_create_sws_context(int src_width, int src_height) {
    // Check if cached context can be reused
    if (sws_context_ && cached_width_ == src_width && cached_height_ == src_height) {
        return sws_context_;
    }

    // Resolution changed or no context exists, create new one
    if (sws_context_) {
        sws_freeContext(static_cast<SwsContext*>(sws_context_));
        sws_context_ = nullptr;
        LOG_INFO("SWS context released due to resolution change");
    }

    SwsContext* sws = sws_getContext(
        src_width, src_height, AV_PIX_FMT_RGBA,
        src_width, src_height, AV_PIX_FMT_NV12,
        SWS_BILINEAR,
        nullptr, nullptr, nullptr);

    if (sws) {
        sws_context_ = sws;
        cached_width_ = src_width;
        cached_height_ = src_height;
        LOG_INFO("SWS context created for " + std::to_string(src_width) + "x" + std::to_string(src_height));
    } else {
        LOG_ERROR("Failed to create SWS context for " + std::to_string(src_width) + "x" + std::to_string(src_height));
    }

    return sws_context_;
}

void CompositorEncoderBridge::release_sws_context() {
    if (sws_context_) {
        sws_freeContext(static_cast<SwsContext*>(sws_context_));
        sws_context_ = nullptr;
        cached_width_ = 0;
        cached_height_ = 0;
        LOG_INFO("SWS context released");
    }
}

// 新增：处理音频引擎的原始数据（使用 media_clock 确保与视频同步）
void CompositorEncoderBridge::on_audio_data_ready(const QByteArray& data, int64_t timestamp) {
    if (!streaming_ || !encoder_) {
        return;
    }

    // 🔧 使用 media_clock 获取相对时间戳（与视频使用同一个时钟）
    // 这样可以确保音视频时间戳同步
    // 注意：传入的 timestamp 参数被忽略，使用 media_clock_ 的值
    auto current_timestamp_us = media_clock_.get_elapsed_time_us();
    auto current_timestamp_ms = current_timestamp_us / 1000;

    // 🔧 OBS 风格 PTS 偏移归零：记录第一帧音频 PTS 并应用偏移
    // 注意：音频和视频必须使用同一个 PTS 基准（first_video_pts_ms_），确保音视频同步
    if (first_video_pts_ms_ == -1) {
        first_video_pts_ms_ = current_timestamp_ms;
        LOG_INFO("[BRIDGE] First audio PTS recorded (video_pts): " + std::to_string(first_video_pts_ms_) + "ms");
    }
    
    if (!streaming_pts_initialized_) {
        streaming_pts_initialized_ = true;
        LOG_INFO("[BRIDGE] Streaming PTS initialized by audio at: " + std::to_string(current_timestamp_ms) + "ms");
    }
    
    // 应用 PTS 偏移，确保音视频使用同一个基准
    int64_t adjusted_timestamp_ms = current_timestamp_ms - first_video_pts_ms_;

    static int audio_frame_count = 0;
    audio_frame_count++;
    if (audio_frame_count <= 3) {
        LOG_INFO("[BRIDGE] on_audio_data_ready #" + std::to_string(audio_frame_count) + 
                 ": raw_pts=" + std::to_string(current_timestamp_ms) + "ms, " +
                 "adjusted_pts=" + std::to_string(adjusted_timestamp_ms) + "ms, " +
                 "offset=" + std::to_string(first_video_pts_ms_) + "ms, " +
                 "data.size=" + std::to_string(data.size()));
    }

    auto* audio_encoder = dynamic_cast<AACEncoder*>(encoder_->get_audio_encoder());
    if (!audio_encoder) {
        return;
    }

    audio_encoder->encode_audio_data(data, adjusted_timestamp_ms);
}

// 新增：处理编码后的音频数据（推送到流）
void CompositorEncoderBridge::on_audio_encoded(const uint8_t* data, int size, int64_t timestamp) {
    if (!stream_pusher_ || !stream_pusher_->is_pushing() || !data || size <= 0) {
        return;
    }

    // 🔇 过滤静音帧和异常帧
    // 6字节AAC帧是已知异常帧，会导致杂音
    // if (size == 6) {
    //     static int silent_6byte_count = 0;
    //     if (++silent_6byte_count <= 3) {
    //         LOG_DEBUG("[BRIDGE] Filtered 6-byte abnormal AAC frame");
    //     }
    //     return;
    // }

    // 🔇 检测静音帧：检查前64字节，如果90%以上都是静音则过滤
    // if (size > 32) {
    //     int check_bytes = (size < 64) ? size : 64;
    //     int near_zero_count = 0;
    //     for (int i = 0; i < check_bytes; i++) {
    //         // 检查是否为0或接近0（小于5）
    //         if (data[i] == 0 || data[i] < 5) {
    //             near_zero_count++;
    //         }
    //     }
    //     int silence_percent = (near_zero_count * 100) / check_bytes;
    //     if (silence_percent > 90) {
    //         static int silent_frame_count = 0;
    //         if (++silent_frame_count <= 5) {
    //             LOG_DEBUG("[BRIDGE] Filtered silent frame: " + std::to_string(silence_percent) +
    //                      "% silent, size=" + std::to_string(size));
    //         }
    //         return;
    //     }
    // }

    // 🔧 诊断音频包
    static int64_t first_audio_pts = -1;
    static int audio_count = 0;
    audio_count++;

    // ✅ 修复负数 PTS：AAC 编码器会有编码延迟（如 -1024），需要修正为 0
    int64_t adjusted_timestamp = timestamp;
    if (timestamp < 0) {
        adjusted_timestamp = 0;
        if (audio_count <= 5) {
            LOG_WARNING("[BRIDGE] Adjusted negative audio PTS from " + std::to_string(timestamp) +
                       " to 0 (AAC encoder delay)");
        }
    }

    if (first_audio_pts == -1) {
        first_audio_pts = adjusted_timestamp;
        LOG_INFO("[BRIDGE] First AUDIO packet: pts=" + std::to_string(adjusted_timestamp) +
                 "ms, size=" + std::to_string(size) +
                 ", media_clock=" + std::to_string(media_clock_.get_elapsed_time_us() / 1000) + "ms");
    } else if (audio_count <= 5) {
        int64_t delta_ms = adjusted_timestamp - first_audio_pts;
        LOG_INFO("[BRIDGE] Audio packet #" + std::to_string(audio_count) +
                 ": pts=" + std::to_string(adjusted_timestamp) +
                 "ms, delta=" + std::to_string(delta_ms) + "ms" +
                 ", media_clock=" + std::to_string(media_clock_.get_elapsed_time_us() / 1000) + "ms");
    }

    // 创建 AVPacket 并复制数据
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        LOG_ERROR("[BRIDGE] Failed to allocate AVPacket for encoded audio");
        return;
    }

    // 复制编码数据到 AVPacket
    if (av_new_packet(pkt, size) < 0) {
        LOG_ERROR("[BRIDGE] Failed to allocate packet data");
        av_packet_free(&pkt);
        return;
    }
    std::memcpy(pkt->data, data, size);

    // ✅ 使用修正后的时间戳（确保音视频同步且避免负数）
    pkt->pts = adjusted_timestamp;
    pkt->dts = adjusted_timestamp;
    // ✅ AAC 帧 duration：1024采样 @ 48kHz ≈ 21.33ms
    pkt->duration = (1024 * 1000) / 48000;

    // 创建编码数据包
    auto packet = std::make_shared<EncodedPacket>();
    packet->type = MediaType::AUDIO;
    packet->pts = adjusted_timestamp;  // ✅ 使用修正后的时间戳（避免负数）
    packet->dts = adjusted_timestamp;
    // ✅ AAC 帧 duration：1024采样 @ 48kHz ≈ 21.33ms
    packet->duration = (1024 * 1000) / 48000;
    packet->pkt = AVPacketPtr(pkt);
    // ✅ 使用毫秒作为 time_base，与 FLV 容器一致
    packet->encoder_time_base = {1, 1000};
    packet->wallclock_us = media_clock_.get_elapsed_time_us();  // 仅用于调试

    // 推送到流
    stream_pusher_->push_packet(packet);
}

// ═══════════════════════════════════════════════════════════════════════
// 🔧 异步编码架构：编码线程池实现
// ═══════════════════════════════════════════════════════════════════════

void CompositorEncoderBridge::start_encoder_threads() {
    if (encoder_threads_running_.load()) {
        LOG_WARNING("[BRIDGE] Encoder threads already running");
        return;
    }
    
    encoder_threads_running_.store(true);
    
    // 启动最多 MAX_ENCODER_THREADS 个编码线程
    int thread_count = MAX_ENCODER_THREADS;
    encoder_threads_.reserve(thread_count);
    
    for (int i = 0; i < thread_count; i++) {
        encoder_threads_.emplace_back(&CompositorEncoderBridge::encoder_thread_func, this, i);
        LOG_INFO("[BRIDGE] Started encoder thread #" + std::to_string(i));
    }
    
    LOG_INFO("[BRIDGE] Encoder thread pool started with " + std::to_string(thread_count) + " threads");
}

void CompositorEncoderBridge::stop_encoder_threads() {
    if (!encoder_threads_running_.load()) {
        return;
    }
    
    encoder_threads_running_.store(false);
    
    // 唤醒所有等待中的编码线程
    encode_queue_cv_.notify_all();
    
    // 等待所有编码线程退出
    for (auto& thread : encoder_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    
    encoder_threads_.clear();
    
    // 清空编码队列
    {
        std::lock_guard<std::mutex> lock(encode_queue_mutex_);
        pre_encode_queue_.clear();
    }
    
    LOG_INFO("[BRIDGE] Encoder thread pool stopped");
}

void CompositorEncoderBridge::encoder_thread_func(int thread_id) {
    LOG_INFO("[BRIDGE] Encoder thread #" + std::to_string(thread_id) + " started");

    while (encoder_threads_running_.load()) {
        PreEncodeVideoFrame pre_frame;

        // 从队列取帧（阻塞等待）
        auto wait_start = std::chrono::high_resolution_clock::now();
        if (!pop_from_encode_queue(pre_frame)) {
            // 队列为空或线程已停止
            continue;
        }
        auto wait_end = std::chrono::high_resolution_clock::now();
        auto wait_time = std::chrono::duration_cast<std::chrono::milliseconds>(wait_end - wait_start).count();

        // 检查帧是否有效
        if (!pre_frame.frame) {
            continue;
        }

        try {
            // 🔧 诊断：记录队列等待时间
            static int encode_count = 0;
            encode_count++;

            auto process_start = std::chrono::high_resolution_clock::now();

            // 编码视频帧
            std::vector<EncodedPacketPtr> video_packets;
            ErrorCode video_result = encoder_->encode_video_frame(pre_frame.frame, video_packets);

            auto process_end = std::chrono::high_resolution_clock::now();
            auto process_time = std::chrono::duration_cast<std::chrono::milliseconds>(process_end - process_start).count();

            // 🔧 诊断：只在异常或特定帧时记录详细日志
            if (wait_time > 100 || process_time > 50 || encode_count <= 3 || encode_count % 50 == 0) {
                LOG_INFO("[BRIDGE][Thread#" + std::to_string(thread_id) + "] Frame #" +
                         std::to_string(pre_frame.frame_count) +
                         " wait=" + std::to_string(wait_time) + "ms" +
                         " encode=" + std::to_string(process_time) + "ms" +
                         " pts=" + std::to_string(pre_frame.pts_ms) + "ms" +
                         " packets=" + std::to_string(video_packets.size()));
            }

            // 编码完成后，推送编码后的包
            if (video_result == ErrorCode::SUCCESS && !video_packets.empty() &&
                stream_pusher_ && stream_pusher_->is_pushing()) {

                for (auto& p : video_packets) {
                    if (!p) continue;
                    p->wallclock_us = pre_frame.wallclock_us;
                    stream_pusher_->push_packet(p);
                }
            }
        } catch (const std::exception& ex) {
            LOG_ERROR("[BRIDGE][Thread#" + std::to_string(thread_id) + "] Exception: " + std::string(ex.what()));
        }
    }

    LOG_INFO("[BRIDGE] Encoder thread #" + std::to_string(thread_id) + " stopped");
}

void CompositorEncoderBridge::push_to_encode_queue(const PreEncodeVideoFrame& frame) {
    std::lock_guard<std::mutex> lock(encode_queue_mutex_);

    // 如果队列满了，丢弃最旧的帧
    if (pre_encode_queue_.size() >= MAX_ENCODE_QUEUE_SIZE) {
        pre_encode_queue_.pop_front();
        LOG_WARNING("[BRIDGE] Encode queue full, dropping oldest frame, queue_size=" + std::to_string(pre_encode_queue_.size()));
    }

    pre_encode_queue_.push_back(frame);

    // 🔧 诊断：只在队列异常时记录
    static int warn_count = 0;
    if (pre_encode_queue_.size() > 10 && warn_count++ < 10) {
        LOG_INFO("[BRIDGE] Queue push: size=" + std::to_string(pre_encode_queue_.size()) +
                 " pts=" + std::to_string(frame.pts_ms) + "ms");
    }

    encode_queue_cv_.notify_one();
}

bool CompositorEncoderBridge::pop_from_encode_queue(PreEncodeVideoFrame& frame) {
    std::unique_lock<std::mutex> lock(encode_queue_mutex_);
    
    // 等待队列有数据或线程停止
    encode_queue_cv_.wait(lock, [this] {
        return !pre_encode_queue_.empty() || !encoder_threads_running_.load();
    });
    
    // 如果线程已停止，返回 false
    if (!encoder_threads_running_.load()) {
        return false;
    }
    
    // 取帧
    if (pre_encode_queue_.empty()) {
        return false;
    }
    
    frame = pre_encode_queue_.front();
    pre_encode_queue_.pop_front();
    
    return true;
}

// ═══════════════════════════════════════════════════════════════
// 插播视频帧同步方法实现
// ═══════════════════════════════════════════════════════════════

void CompositorEncoderBridge::attach_insert_video_source(MediaFileSource* source) {
    if (!source) return;

    // 获取当前推流的媒体时钟作为时间基准
    int64_t current_stream_time_us = media_clock_.get_elapsed_time_us();

    // 注入到插播视频源
    source->set_external_time_base(current_stream_time_us);

    LOG_INFO("[CompositorEncoderBridge] Attached insert video with time base: " +
             std::to_string(current_stream_time_us) + "us");

    // 保存 source_id 用于直接更新 Compositor
    const std::string source_id = source->get_id();

    // 设置帧回调，解码后同时：1) 推送到画布渲染 2) 推入同步器用于编码
    source->set_frame_ready_callback_with_pts(
        [this, source](std::shared_ptr<VideoFrame> frame, int64_t pts_ms) {
            if (!frame || !frame->data) return;

            // 诊断：统计回调触发频率
            static int64_t last_callback_time = 0;
            static int callback_count = 0;
            int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            callback_count++;
            if (now - last_callback_time >= 1000) {
                LOG_INFO("[CALLBACK] insert_video fps: " + std::to_string(callback_count));
                callback_count = 0;
                last_callback_time = now;
            }

            // 1) 直接传递原始数据，避免拷贝
            // 注意：VideoFrame 的生命周期由 scheduler 线程控制，
            // 在这里拷贝一份是必要的，因为 VideoFrame 可能会被复用
            // 但我们只需要在 push_frame 中做一次拷贝就够了
            source->push_frame_with_raw_data(frame);

            // 2) 推入同步器用于编码（推流需要）
            if (!insert_video_synchronizer_->push_frame(frame, pts_ms)) {
                // 编码队列满则丢帧（这是正常的，不打印警告避免日志刷屏）
            }
        }
    );
}

void CompositorEncoderBridge::detach_insert_video_source(MediaFileSource* source) {
    if (!source) return;

    // 重置外部时间基准
    source->reset_external_time_base();

    // 清除 MediaFileSource 中存储的帧（避免旧帧残留）
    source->push_frame(QImage());

    // 清除同步器队列
    insert_video_synchronizer_->clear();

    LOG_INFO("[CompositorEncoderBridge] Detached insert video source");
}

bool CompositorEncoderBridge::pop_insert_video_frame(SyncedVideoFrame& out_frame) {
    // 非阻塞获取帧（timeout = 0）
    return insert_video_synchronizer_->try_pop_frame(out_frame);
}

bool CompositorEncoderBridge::push_insert_video_frame(std::shared_ptr<VideoFrame> frame, int64_t pts_ms) {
    if (!frame || !frame->data) return false;
    return insert_video_synchronizer_->push_frame(frame, pts_ms);
}

} // namespace live_assistant
