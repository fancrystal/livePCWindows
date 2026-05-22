#define NOMINMAX
#include <cmath>
#include "scene_manager/compositor_encoder_bridge.h"
#include "common/log.h"
#include "common/error.h"
#include "video_engine/video_engine.h"
#include "audio_engine/audio_engine.h"
#include "encoder/audio_encoder.h"
#include "media_pipeline/media_file_source.h"
#include "scene_manager/gpu_compositor.h"
#include "scene_manager/gpu_color_converter.h"
#include "scene_manager/shared_d3d_device.h"
#include <d3d11.h>

#include <QImage>
#include <QPainter>
#include <cstring>
#include <cstdlib>
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
            
            if (!bridge->capture_thread_stop_ && bridge->running_.load(std::memory_order_acquire)) {
                // 仅在上一帧已被主线程消费后才投递新帧。
                // 若主线程繁忙（UI/事件处理），跳过本次投递以避免 QueuedConnection 积压：
                // 积压会导致两帧在极短时间内连续编码，产生 PTS 突刺和播放端卡顿。
                bool expected = false;
                if (bridge->frame_pending_.compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel)) {
                    bridge->last_capture_time_ = std::chrono::steady_clock::now();
                    QMetaObject::invokeMethod(bridge, "on_encode_timer", Qt::QueuedConnection);
                }
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

void CompositorEncoderBridge::set_canvas_renderer(CanvasRenderer* renderer, std::shared_ptr<Scene> scene) {
    // 非拥有裸指针：lifetime 由 CanvasWidget::renderer_（unique_ptr）保证
    canvas_renderer_ = renderer;
    current_scene_ = scene;
    LOG_INFO("CanvasRenderer set for encoder bridge (fallback mode)");
}

void CompositorEncoderBridge::set_gpu_compositor(std::shared_ptr<GpuCompositor> gpu_compositor) {
    gpu_compositor_ = gpu_compositor;
    LOG_INFO("GpuCompositor set for encoder bridge");

    // Phase 1 多 GPU 路径决策：按 vendor 决定是否启用 CS BGRA→NV12 路径。
    //   - NVIDIA / AMD: 无 Intel RC/CCS 问题，开启 GPU 路径
    //   - Intel       : 仍保留 CPU 兜底（QSV+CS 路径需要 GpuNv12Copier 支持，后续接入）
    //   - Other / Unknown: 安全起见走 CPU
    // 环境变量 LIVEASSISTANT_FORCE_CPU_COMPOSITOR=1 可强制走 CPU 路径方便排查。
    auto vendor = SharedD3D11Device::instance().vendor();
    bool force_cpu = false;
    if (const char* env = std::getenv("LIVEASSISTANT_FORCE_CPU_COMPOSITOR")) {
        force_cpu = (env[0] != '\0' && env[0] != '0');
    }
    if (force_cpu) {
        gpu_path_enabled_ = false;
        LOG_INFO("[BRIDGE] GPU path disabled by LIVEASSISTANT_FORCE_CPU_COMPOSITOR");
    } else if (vendor == SharedD3D11Device::GpuVendor::NVIDIA ||
               vendor == SharedD3D11Device::GpuVendor::AMD) {
        gpu_path_enabled_ = true;
        LOG_INFO(std::string("[BRIDGE] GPU compositor path enabled for vendor=") +
                 (vendor == SharedD3D11Device::GpuVendor::NVIDIA ? "NVIDIA" : "AMD") +
                 " (CS BGRA->NV12 + CPU readback)");
    } else {
        gpu_path_enabled_ = false;
        LOG_INFO("[BRIDGE] GPU path disabled (vendor=Intel/Unknown, using CPU compositor path for safety)");
    }
}

void CompositorEncoderBridge::set_gpu_color_converter(std::shared_ptr<GpuColorConverter> gpu_color_converter) {
    gpu_color_converter_ = gpu_color_converter;
    LOG_INFO("GpuColorConverter set for encoder bridge");
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
    if (stream_pusher_) {
        // 重连成功后通知视频编码器立刻强制输出 IDR 关键帧。
        // callback 在 StreamPusher 的推流线程中调用，force_keyframe() 是线程安全的原子操作。
        stream_pusher_->set_reconnect_callback([this]() {
            if (encoder_) {
                encoder_->force_keyframe();
                LOG_INFO("[BRIDGE] Forced IDR keyframe after RTMP reconnect");
            }
        });
        stream_pusher_->set_reconnecting_callback([this](int attempt, int max_attempts) {
            QMetaObject::invokeMethod(this, [this, attempt, max_attempts]() {
                emit streaming_reconnecting(attempt, max_attempts);
            }, Qt::QueuedConnection);
        });
    }
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
                this, &CompositorEncoderBridge::on_audio_data_ready, Qt::DirectConnection);
        LOG_INFO("Audio engine set for encoder bridge and signal connected (DirectConnection)");
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
        
        // 🔧 修复竖屏推流绿屏问题：确保视频编码器使用最新的分辨率配置
        // 问题：ensure_video_encoder_initialized() 可能使用过期的 video_config_
        // 导致 SPS/PPS 中的分辨率与实际画面不符，拉流端解码失败显示绿屏
        VideoEncoderConfig current_video_config = encoder_->get_video_config();
        if (current_video_config.width != width_ || current_video_config.height != height_) {
            // 分辨率配置已变更（如横屏→竖屏切换），需要重新初始化编码器
            LOG_INFO("[BRIDGE] Video config mismatch: encoder=" + std::to_string(current_video_config.width) + "x" + 
                     std::to_string(current_video_config.height) + ", current=" + std::to_string(width_) + "x" + std::to_string(height_));
            current_video_config.width = width_;
            current_video_config.height = height_;
            ErrorCode reinit_result = encoder_->reinitialize_video_encoder(current_video_config);
            if (reinit_result != ErrorCode::SUCCESS) {
                if (a_par) avcodec_parameters_free(&a_par);
                LOG_ERROR("[BRIDGE] Failed to reinitialize video encoder with updated resolution");
                emit streaming_error(QString("Failed to reinitialize video encoder: %1").arg(static_cast<int>(reinit_result)));
                return false;
            }
            LOG_INFO("[BRIDGE] Video encoder reinitialized with resolution: " + std::to_string(width_) + "x" + std::to_string(height_));
            // reinitialize_video_encoder() only stores config when encoder is not yet
            // active; call ensure_video_encoder_initialized() to actually create it.
            ErrorCode ensure_result = encoder_->ensure_video_encoder_initialized();
            if (ensure_result != ErrorCode::SUCCESS) {
                if (a_par) avcodec_parameters_free(&a_par);
                LOG_ERROR("[BRIDGE] Failed to initialize video encoder after resolution update");
                emit streaming_error(QString("Failed to start streaming: %1").arg(static_cast<int>(ensure_result)));
                return false;
            }
        } else {
            // 配置匹配，只需确保编码器已初始化
            ErrorCode video_init_result = encoder_->ensure_video_encoder_initialized();
            if (video_init_result != ErrorCode::SUCCESS) {
                if (a_par) avcodec_parameters_free(&a_par);
                LOG_ERROR("[BRIDGE] Failed to initialize video encoder for streaming");
                emit streaming_error(QString("Failed to start streaming: %1").arg(static_cast<int>(video_init_result)));
                return false;
            }
        }
        
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
                // Force actual creation if encoder was not yet active
                r = encoder_->ensure_video_encoder_initialized();
            }
            if (r == ErrorCode::SUCCESS) {
                v_par = encoder_->get_video_codec_parameters();
                v_tb = encoder_->get_video_time_base();
            }
        }

        if (!a_par || !v_par || a_tb.den <= 0 || v_tb.den <= 0) {
            if (a_par) avcodec_parameters_free(&a_par);
            if (v_par) avcodec_parameters_free(&v_par);
            encoder_->shutdown_video_encoder();
            LOG_ERROR("[BRIDGE] Audio/Video codec parameters unavailable or invalid, cannot start streaming");
            emit streaming_error(QString("Failed to start streaming: %1").arg(static_cast<int>(ErrorCode::INVALID_STATE)));
            return false;
        }

        ErrorCode ra = stream_pusher_->register_audio_stream(a_par, a_tb);
        ErrorCode rv = stream_pusher_->register_video_stream(v_par, v_tb);

        avcodec_parameters_free(&a_par);
        avcodec_parameters_free(&v_par);

        if (ra != ErrorCode::SUCCESS || rv != ErrorCode::SUCCESS) {
            encoder_->shutdown_video_encoder();
            LOG_ERROR("[BRIDGE] Failed to register audio/video streams");
            emit streaming_error(QString("Failed to start streaming: %1").arg(static_cast<int>(ErrorCode::INVALID_STATE)));
            return false;
        }
    }

    running_ = false;
    // 不再需要停止 QTimer
    // 注意：移除了 50ms sleep，避免这段时间内 WASAPI 捕获的音频因 streaming_=false 被丢弃
    // 这会导致推流开始时第一个音节被截断

    if (encoder_) {
        encoder_->reset_audio_encoder();
        // 🔧 强制下一帧视频为关键帧，确保推流开始时有 IDR 帧可供解码
        encoder_->force_keyframe();
        LOG_INFO("[BRIDGE] Requested keyframe for streaming start");
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
    {
        std::lock_guard<std::mutex> alock(audio_pts_mutex_);
        first_video_pts_ms_ = -1;
        streaming_pts_initialized_ = false;
        audio_pts_clock_offset_ms_ = INT64_MIN;
    }
    // 视频帧计数器 PTS 基准
    video_pts_base_us_ = -1;
    video_pts_initial_frame_ = -1;
    // 重置漂移监控状态
    audio_total_samples_received_ = 0;
    last_drift_check_ms_ = 0;
    audio_frame_count_ = 0;
    streaming_error_emitted_.store(false);
    first_audio_timestamp_ms_ = -1;
    media_clock_start_us_ = 0;
    // 🔧 重置音频诊断计数（原来是 static 本地变量，每次推流必须重置）
    diag_first_audio_pts_ = -1;
    diag_audio_count_ = 0;
    LOG_INFO("[BRIDGE] PTS offset reset, waiting for first frame");

    media_clock_.start();
    // Record steady_clock epoch that matches media_clock_.start() so we can
    // convert mixer emit-time steady_clock timestamps to bridge PTS later.
    streaming_start_steady_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    LOG_INFO("[BRIDGE] Media clock started, streaming_start_steady_us=" +
             std::to_string(streaming_start_steady_us_));

    start_encoder_threads();

    ErrorCode start_result = stream_pusher_->start();
    if (start_result != ErrorCode::SUCCESS) {
        LOG_ERROR("Failed to start streaming to: " + url);
        // Clean up before emitting the error signal.  The signal is connected with
        // QueuedConnection so on_streaming_error runs after we return and release
        // state_mutex_, but emit the error AFTER cleanup to keep state consistent.
        stop_encoder_threads();
        if (encoder_) {
            encoder_->shutdown_video_encoder();
        }
        media_clock_.stop();
        QString err_msg = QString("Failed to start streaming: %1").arg(static_cast<int>(start_result));
        emit streaming_error(err_msg);
        return false;
    }

    running_ = true;
    capture_fps_.store(fps_);
    last_capture_time_ = std::chrono::steady_clock::now();
    // 不再需要启动 QTimer，工作线程已经在运行

    LOG_INFO("[BRIDGE] Encoding restarted, media_clock: " + std::to_string(media_clock_.get_elapsed_time_us() / 1000) + "ms");

    streaming_.store(true);
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

    // 先将 streaming_ 置为 false，阻止 on_audio_data_ready 继续向音频编码器投递数据
    streaming_.store(false);

    // ═══════════════════════════════════════════════════════════════
    // 🔧 停止编码线程池（队列已在 stop_encoder_threads 内预先清空）
    // ═══════════════════════════════════════════════════════════════
    stop();  // stops capture thread + encoder threads + media_clock

    // 释放 sws_context_（RGBA→NV12 色彩转换上下文），避免跨推流会话内存驻留
    release_sws_context();

    if (stream_pusher_) {
        stream_pusher_->stop();
    }

    if (encoder_) {
        encoder_->shutdown_video_encoder();
        // 清空音频编码器的待处理队列，释放 QByteArray 音频数据
        auto* audio_enc = dynamic_cast<AACEncoder*>(encoder_->get_audio_encoder());
        if (audio_enc) {
            audio_enc->clear_encode_queue();
        }
    }
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
    // 无论 streaming_ 状态如何，立刻清除 pending 标志。
    // 必须在函数最顶部清除：若 streaming_=false 时触发（start_streaming() 临时将 running_
    // 置 false 的窗口期），capture_and_queue_frame() 不会被调用，flag 会永久卡在 true，
    // 导致捕获线程再也不投递新帧（视频完全停止）。
    frame_pending_.store(false, std::memory_order_release);

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
    if (streaming_) {
        capture_and_queue_frame();
    }
}

void CompositorEncoderBridge::capture_and_queue_frame() {
    // 主线程已接收到本帧投递，允许捕获线程发出下一帧。
    frame_pending_.store(false, std::memory_order_release);

    try {
        // [DIAG] Frame capture start time
        auto frame_start = std::chrono::high_resolution_clock::now();

        video_frame_count_++;

        auto current_pts_ms = media_clock_.get_elapsed_time_us() / 1000;

        // Video PTS uses wall-clock elapsed time (same source as audio PTS).
        // 帧号 × 帧时长，保证每个编码帧 PTS 等间隔（30fps = 33.33ms）
        // first_video_pts_ms_ 仅用于与音频的相对同步（第一帧对齐）
        // Protected by audio_pts_mutex_ because on_audio_data_ready (mixer thread)
        // reads this value concurrently.
        {
            std::lock_guard<std::mutex> alock(audio_pts_mutex_);
            if (first_video_pts_ms_ == -1) {
                first_video_pts_ms_ = current_pts_ms;
                video_pts_base_us_ = current_pts_ms * 1000LL;
                video_pts_initial_frame_ = video_frame_count_;
                LOG_INFO("[BRIDGE] First video PTS recorded: " + std::to_string(first_video_pts_ms_) + "ms");
            }

            // 如果 streaming_pts_initialized_ 还未初始化（音频未到达），现在初始化
            if (!streaming_pts_initialized_) {
                streaming_pts_initialized_ = true;
                LOG_INFO("[BRIDGE] Streaming PTS initialized by video at: " + std::to_string(current_pts_ms) + "ms");
            }
        }

        // Use wall-clock elapsed time for video PTS so that video and audio
        // share the same time reference (both derived from media_clock_ / steady_clock).
        // Frame-counter PTS (frame_offset × 33333μs) drifts whenever the compositor
        // drops or delays a frame — audio keeps running at real speed, causing
        // long-term A/V desync (~12 ms/s observed over 4 minutes).
        int64_t adjusted_pts_ms = media_clock_.get_elapsed_time_us() / 1000LL;

        // raw_pts 仅用于诊断，不参与实际 PTS 计算
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
            // GPU 路径：若 capture_compositor_frame() 走了 GPU 路径，则传递 NV12 纹理引用
            pre_frame.gpu_nv12_ref = pending_gpu_nv12_ref_;
            pending_gpu_nv12_ref_ = GpuTextureRef{};  // 消费后清零

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
    // 多 GPU 渲染流水线（Phase 1）
    //
    // 路径选择：
    //   * gpu_path_enabled_ = true  → 走 GpuCompositor + CsBgraToNv12（CS BGRA→NV12）
    //   * gpu_path_enabled_ = false → 走下方 CPU Compositor / CanvasRenderer
    //
    // gpu_path_enabled_ 在 set_gpu_compositor() 中按 vendor 决定：
    //   NVIDIA / AMD → 默认启用（CS via SRV 透明绕过 Intel RC/CCS 限制）
    //   Intel / Unknown → 默认走 CPU（QSV 零拷贝路径待 GpuNv12Copier 接入）
    //
    // 注：GpuCompositor 的场景层（GpuCompositorLayer）当前尚未由 WGC/Media 源喂入，
    // 因此即便 gpu_path_enabled_=true，本阶段仍会落到 CPU 合成路径。
    // CsBgraToNv12 已就绪，等待后续阶段把 WGC/Media 源对接到 GpuCompositor。
    // ═══════════════════════════════════════════════════════════════════
    pending_gpu_nv12_ref_ = GpuTextureRef{};

    if (gpu_path_enabled_ && gpu_compositor_) {
        // 惰性初始化 BGRA→NV12 Compute Shader（尺寸变化时重建）
        if (!cs_bgra_to_nv12_ ||
            gpu_path_init_w_ != width_ ||
            gpu_path_init_h_ != height_) {
            if (!gpu_path_init_failed_) {
                cs_bgra_to_nv12_ = std::make_unique<CsBgraToNv12>();
                if (cs_bgra_to_nv12_->initialize(width_, height_)) {
                    gpu_path_init_w_ = width_;
                    gpu_path_init_h_ = height_;
                    LOG_INFO("[BRIDGE] CsBgraToNv12 initialized for GPU path: " +
                             std::to_string(width_) + "x" + std::to_string(height_));
                } else {
                    LOG_WARNING("[BRIDGE] CsBgraToNv12 init failed, falling back to CPU path");
                    cs_bgra_to_nv12_.reset();
                    gpu_path_init_failed_ = true;
                }
            }
        }

        // 若 GpuCompositor 已经有 BGRA 输出（compose() 被调用过），直接 CS 转 NV12。
        if (cs_bgra_to_nv12_ && cs_bgra_to_nv12_->is_initialized()) {
            ID3D11ShaderResourceView* bgra_srv = gpu_compositor_->output_srv();
            if (bgra_srv) {
                auto frame = cs_bgra_to_nv12_->convert_to_cpu(
                    bgra_srv, media_clock_.now_us() / 1000);
                if (frame) {
                    return frame;
                }
                LOG_WARNING("[BRIDGE] CS BGRA->NV12 convert failed, falling back to CPU path");
            }
            // 没有 BGRA SRV：场景源还未对接到 GpuCompositor，落到 CPU 路径。
            static bool layers_missing_logged = false;
            if (!layers_missing_logged) {
                LOG_INFO("[BRIDGE] GpuCompositor has no BGRA output yet (scene layers not wired); using CPU path");
                layers_missing_logged = true;
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // 优先使用 Compositor（如果所有源都正确更新了帧到 Compositor）
    // ═══════════════════════════════════════════════════════════════════
    if (compositor_) {
        // Use ARGB32 format to match WGC/BGRA output
        const QImage img = compositor_->render_to_image(width_, height_);
        if (!img.isNull()) {
            return convert_qimage_to_video_frame(img);
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // 回退方案：使用 CanvasRenderer（能正确处理所有类型的源）
    // ═══════════════════════════════════════════════════════════════════
    if (canvas_renderer_ && current_scene_) {
        QImage img(width_, height_, QImage::Format_ARGB32);
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

    // ═══════════════════════════════════════════════════════════════════
    // 诊断日志：检查 QImage stride 是否与预期一致
    // QImage::Format_ARGB32 每个像素 4 字节，所以 bytesPerLine 应该等于 width * 4
    // 但某些情况下 Qt 会自动对齐到特定边界，导致 bytesPerLine > width * 4
    // 这可能导致 sws_scale 读取到错误的内存数据（绿屏问题）
    // ═══════════════════════════════════════════════════════════════════
    const int expected_stride = width_ * 4;
    const int actual_stride = img.bytesPerLine();
    if (actual_stride != expected_stride) {
        LOG_WARNING("[BRIDGE] QImage stride mismatch! width_=" + std::to_string(width_) +
                    ", img.width()=" + std::to_string(img.width()) +
                    ", expected_stride=" + std::to_string(expected_stride) +
                    ", actual_stride=" + std::to_string(actual_stride) +
                    ", diff=" + std::to_string(actual_stride - expected_stride));
    } else {
        LOG_DEBUG("[BRIDGE] QImage stride OK: " + std::to_string(actual_stride));
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

    // 使��� img.bytesPerLine() 作为源 stride（可能大于 width * 4）
    const uint8_t* src_slices[1] = { reinterpret_cast<const uint8_t*>(img.constBits()) };
    int src_strides[1] = { actual_stride };

    uint8_t* dst_slices[2] = { frame->data.get(), frame->data_uv.get() };
    int dst_strides[2] = { frame->stride, frame->stride_uv };

    // [DIAG] SWS conversion performance
    auto sws_start = std::chrono::high_resolution_clock::now();
    sws_scale(sws, src_slices, src_strides, 0, height_, dst_slices, dst_strides);
    auto sws_end = std::chrono::high_resolution_clock::now();
    auto sws_us = std::chrono::duration_cast<std::chrono::microseconds>(sws_end - sws_start).count();
    LOG_DEBUG("[BRIDGE] SWS RGBA->NV12: " + std::to_string(sws_us) + "us, " +
              std::to_string(width_) + "x" + std::to_string(height_) +
              ", src_stride=" + std::to_string(actual_stride));

    // 诊断：检查 NV12 UV 平面是否全为 0（UV=0 会导致绿色偏移，是真正的数据丢失）
    // 注意：UV=0x80(128) 是"无色/中性灰"，对空场景/纯色背景属于正常值，不应报警
    if (frame->data_uv && uv_size > 0) {
        uint32_t zero_count = 0;
        const uint8_t* uv_data = frame->data_uv.get();
        for (int i = 0; i < std::min(uv_size, 64); i++) {
            if (uv_data[i] == 0) zero_count++;
        }
        if (zero_count > 32) {
            static uint32_t uv_zero_warn_count = 0;
            ++uv_zero_warn_count;
            if (uv_zero_warn_count == 1 || uv_zero_warn_count % 300 == 0) {
                LOG_WARNING("[BRIDGE] NV12 UV plane mostly zero (zero_count=" + std::to_string(zero_count) +
                            "/64), possible UV data loss. occurrence=" + std::to_string(uv_zero_warn_count));
            }
        }
    }

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

    // QImage::Format_ARGB32 is BGRA in memory (little-endian), use AV_PIX_FMT_BGRA
    SwsContext* sws = sws_getContext(
        src_width, src_height, AV_PIX_FMT_BGRA,
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
// NOTE: runs in the mixer thread via Qt::DirectConnection — all accesses to
// shared state must be protected by audio_pts_mutex_.
void CompositorEncoderBridge::on_audio_data_ready(const QByteArray& data, int64_t timestamp) {
    if (!streaming_.load() || !encoder_) {
        return;
    }

    // Convert the mixer's emit-time steady_clock timestamp (absolute microseconds)
    // to bridge-relative milliseconds.  With DirectConnection this conversion stays
    // on the mixer thread, so the timestamp remains aligned with the production time
    // of the audio block instead of a later delivery time on another thread.
    int64_t current_timestamp_ms = 0;
    if (streaming_start_steady_us_ > 0 && timestamp > 0) {
        current_timestamp_ms = (timestamp - streaming_start_steady_us_) / 1000LL;
        if (current_timestamp_ms < 0) {
            current_timestamp_ms = 0;  // guard against tiny clock skew at startup
        }
    } else {
        // Fallback: read bridge clock (only happens if start_streaming not yet called)
        current_timestamp_ms = media_clock_.get_elapsed_time_us() / 1000;
    }

    std::unique_lock<std::mutex> audio_lock(audio_pts_mutex_);

    // Lock PTS alignment on the FIRST audio frame (including any WASAPI warm-up
    // silence).  Previously we skipped silent frames until real signal arrived,
    // but that caused content misalignment: video PTS=0 captured real-world
    // moment T, while audio PTS=0 captured moment T+150ms.  The WASAPI warm-up
    // silence is at most a few frames and is imperceptible; encoding it is far
    // better than a 100-300 ms A/V content offset.
    if (audio_pts_clock_offset_ms_ == INT64_MIN) {
        if (first_video_pts_ms_ >= 0) {
            audio_pts_clock_offset_ms_ = current_timestamp_ms - first_video_pts_ms_;
            LOG_INFO("[BRIDGE] Audio PTS aligned: clock=" + std::to_string(current_timestamp_ms) +
                     "ms, video_origin=" + std::to_string(first_video_pts_ms_) +
                     "ms, offset=" + std::to_string(audio_pts_clock_offset_ms_) + "ms");
        } else {
            audio_pts_clock_offset_ms_ = 0;
        }
    }
    current_timestamp_ms -= audio_pts_clock_offset_ms_;

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

    // 音频时钟漂移监控（仅诊断，不修正 PTS）
    // 音视频 PTS 均来自同一个 media_clock，天然同步，无需 PTS 层面的修正。
    // 修正 PTS 会导致时间戳回退，触发 AACEncoder 清空缓冲区和 EINVAL 错误。
    {
        // 使用 AudioEngine 的实际声道数，避免 channels 硬编码导致漂移误报
        int channels = (audio_engine_ && audio_engine_->get_channels() > 0)
                       ? audio_engine_->get_channels() : 1;
        int bytes_per_sample = sizeof(float);
        int64_t samples_in_this_chunk = data.size() / (channels * bytes_per_sample);
        audio_total_samples_received_ += samples_in_this_chunk;

        int64_t elapsed_ms = adjusted_timestamp_ms;
        if (elapsed_ms > 0 && elapsed_ms - last_drift_check_ms_ >= DRIFT_CHECK_INTERVAL_MS) {
            last_drift_check_ms_ = elapsed_ms;

            int64_t expected_samples = elapsed_ms * 48LL;  // 48000 Hz = 48 samples/ms
            int64_t actual_samples = audio_total_samples_received_;
            int64_t sample_diff = actual_samples - expected_samples;
            int64_t drift_us = sample_diff * 1000000LL / 48000LL;

            static int64_t last_drift_log_ms = 0;
            if (elapsed_ms - last_drift_log_ms >= 60000 || std::abs(drift_us) > DRIFT_THRESHOLD_US) {
                last_drift_log_ms = elapsed_ms;
                LOG_INFO("[BRIDGE][AV-SYNC] Drift monitor at " + std::to_string(elapsed_ms / 1000) + "s: "
                         "expected_samples=" + std::to_string(expected_samples) +
                         ", actual_samples=" + std::to_string(actual_samples) +
                         ", drift=" + std::to_string(drift_us / 1000.0) + "ms");
            }
        }
    }

    audio_frame_count_++;
    if (audio_frame_count_ <= 3) {
        LOG_INFO("[BRIDGE] on_audio_data_ready #" + std::to_string(audio_frame_count_) +
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
void CompositorEncoderBridge::on_audio_encoded(const QByteArray& encoded, int64_t timestamp) {
    if (!stream_pusher_ || !stream_pusher_->is_pushing() || encoded.isEmpty()) {
        return;
    }
    const int size = encoded.size();

    // Only drop truly empty packets. Small AAC packets are valid here; logs show
    // regular 6-byte packets after startup, and discarding them starves the muxer
    // so playback buffers forever.
    if (size <= 0) {
        return;
    }

    // 🔧 诊断音频包（使用成员变量，每次推流重置，避免 static 跨会话污染）
    diag_audio_count_++;

    // ✅ 修复负数 PTS：AAC 编码器会有编码延迟（如 -1024），需要修正为 0
    int64_t adjusted_timestamp = timestamp;
    if (timestamp < 0) {
        adjusted_timestamp = 0;
        if (diag_audio_count_ <= 5) {
            LOG_WARNING("[BRIDGE] Adjusted negative audio PTS from " + std::to_string(timestamp) +
                       " to 0 (AAC encoder delay)");
        }
    }

    if (diag_first_audio_pts_ == -1) {
        diag_first_audio_pts_ = adjusted_timestamp;
        LOG_INFO("[BRIDGE] First AUDIO packet: pts=" + std::to_string(adjusted_timestamp) +
                 "ms, size=" + std::to_string(size) +
                 ", media_clock=" + std::to_string(media_clock_.get_elapsed_time_us() / 1000) + "ms");
    } else if (diag_audio_count_ <= 5) {
        int64_t delta_ms = adjusted_timestamp - diag_first_audio_pts_;
        LOG_INFO("[BRIDGE] Audio packet #" + std::to_string(diag_audio_count_) +
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
    std::memcpy(pkt->data, encoded.constData(), size);

    // 修正 AAC 帧 duration：使用 double 避免整数除法截断
    // 正确值：1024 / 48000 = 0.0213333s = 21.3333ms
    // 旧公式 (1024 * 1000) / 48000 = 21 导致每帧少 0.333ms，30秒累积 ~10ms 漂移
    constexpr double AAC_DURATION_MS = 1024.0 * 1000.0 / 48000.0;  // 21.3333...ms

    // ✅ 使用修正后的时间戳（确保音视频同步且避免负数）
    pkt->pts = adjusted_timestamp;
    pkt->dts = adjusted_timestamp;
    // ✅ AAC 帧 duration：1024采样 @ 48kHz ≈ 21.33ms（使用 double 避免截断）
    pkt->duration = static_cast<int64_t>(AAC_DURATION_MS);

    // 创建编码数据包
    auto packet = std::make_shared<EncodedPacket>();
    packet->type = MediaType::AUDIO;
    packet->pts = adjusted_timestamp;  // ✅ 使用修正后的时间戳（避免负数）
    packet->dts = adjusted_timestamp;
    // ✅ AAC 帧 duration：1024采样 @ 48kHz ≈ 21.33ms（使用 double 避免截断）
    packet->duration = static_cast<int64_t>(AAC_DURATION_MS);
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

    // 先清空队列：让 drain 循环立即退出，避免主线程在 join() 中长时间阻塞。
    // 阻塞期间 WGC/摄像头仍在产帧，会将大量 QImage 堆积到 Qt 事件队列（每帧 ~8MB），
    // 导致停流后内存快速上涨。清空队列后 join() 可在几毫秒内返回。
    {
        std::lock_guard<std::mutex> lock(encode_queue_mutex_);
        pre_encode_queue_.clear();
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

            // 编码视频帧：优先走 GPU 纹理直编路径（Phase 4），回退 CPU 路径
            std::vector<EncodedPacketPtr> video_packets;
            ErrorCode video_result = ErrorCode::INVALID_STATE;

            if (pre_frame.gpu_nv12_ref.is_valid() && encoder_->is_gpu_texture_encode_available()) {
                // GPU 路径：直接编码 NV12 纹理，跳过 sws_scale + av_hwframe_transfer_data
                video_result = encoder_->encode_video_gpu_texture(
                    pre_frame.gpu_nv12_ref.texture.get(), pre_frame.pts_ms, video_packets);
                if (video_result != ErrorCode::SUCCESS) {
                    LOG_WARNING("[BRIDGE][Thread#" + std::to_string(thread_id) +
                                "] GPU texture encode failed (pts=" + std::to_string(pre_frame.pts_ms) +
                                "), falling back to CPU path");
                    video_result = encoder_->encode_video_frame(pre_frame.frame, video_packets);
                }
            } else {
                // CPU 路径（回退）
                video_result = encoder_->encode_video_frame(pre_frame.frame, video_packets);
            }

            auto process_end = std::chrono::high_resolution_clock::now();
            auto process_time = std::chrono::duration_cast<std::chrono::milliseconds>(process_end - process_start).count();

            // 🔧 诊断：只在异常或特定帧时记录详细日志
            if (wait_time > 100 || process_time > 50 || encode_count <= 3 || encode_count % 50 == 0) {
                LOG_INFO("[BRIDGE][Thread#" + std::to_string(thread_id) + "] Frame #" +
                         std::to_string(pre_frame.frame_count) +
                         (pre_frame.gpu_nv12_ref.is_valid() ? " [GPU]" : " [CPU]") +
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

            // 检查推流器是否进入错误状态（重连失败等）
            if (stream_pusher_ && stream_pusher_->is_in_error()) {
                LOG_ERROR("[BRIDGE] Stream pusher entered error state, stopping streaming");
                bool expected = false;
                if (streaming_error_emitted_.compare_exchange_strong(expected, true)) {
                    QMetaObject::invokeMethod(this, [this]() {
                        emit streaming_error(QString::fromUtf8("推流断线重连失败，已停止推流"));
                        stop_streaming();
                    }, Qt::QueuedConnection);
                }
                break;
            }
        } catch (const std::exception& ex) {
            LOG_ERROR("[BRIDGE][Thread#" + std::to_string(thread_id) + "] Exception: " + std::string(ex.what()));
        }
    }

    // Drain remaining frames in queue after stop signal, so no frames are lost.
    // The push side is already stopped (capture thread stopped by stop()), so no new frames arrive.
    int drained = 0;
    while (true) {
        PreEncodeVideoFrame pre_frame;
        {
            std::lock_guard<std::mutex> lock(encode_queue_mutex_);
            if (pre_encode_queue_.empty()) {
                break;
            }
            pre_frame = pre_encode_queue_.front();
            pre_encode_queue_.pop_front();
        }

        if (!pre_frame.frame) continue;

        std::vector<EncodedPacketPtr> video_packets;
        ErrorCode drain_result = encoder_->encode_video_frame(pre_frame.frame, video_packets);
        if (drain_result == ErrorCode::SUCCESS && !video_packets.empty() &&
            stream_pusher_ && stream_pusher_->is_pushing()) {
            for (auto& p : video_packets) {
                if (!p) continue;
                p->wallclock_us = pre_frame.wallclock_us;
                stream_pusher_->push_packet(p);
            }
        }

        // drain 阶段已经在停止流程中，不再重复 emit error
        if (stream_pusher_ && stream_pusher_->is_in_error()) {
            break;
        }
        drained++;
    }
    if (drained > 0) {
        LOG_INFO("[BRIDGE][Thread#" + std::to_string(thread_id) + "] Drained " + std::to_string(drained) + " remaining frames before exit");
    }

    LOG_INFO("[BRIDGE] Encoder thread #" + std::to_string(thread_id) + " stopped");
}

void CompositorEncoderBridge::push_to_encode_queue(const PreEncodeVideoFrame& frame) {
    std::lock_guard<std::mutex> lock(encode_queue_mutex_);

    // 如果队列满了，丢弃最新帧（而非最旧帧）
    // 原因：最旧帧可能正在被编码线程消费或即将被取出编码，
    // 丢弃最旧帧会导致PTS跳变和画面跳跃
    if (pre_encode_queue_.size() >= MAX_ENCODE_QUEUE_SIZE) {
        static int drop_count = 0;
        drop_count++;
        if (drop_count <= 20 || drop_count % 100 == 0) {
            LOG_WARNING("[BRIDGE] Encode queue full, dropping NEWEST frame #" +
                       std::to_string(frame.frame_count) + " (total dropped: " +
                       std::to_string(drop_count) + ")");
        }
        return;
    }

    pre_encode_queue_.push_back(frame);

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

    // 不在 attach 时设置时间基准：
    // seek_and_restart + skip 可能需要 0.5~1 秒，期间 audio mix 线程的 MediaClock 已推进。
    // 若在 attach 时记录 base，首帧视频 PTS = attach_time，而音频 PTS ≈ attach_time + skip_duration，
    // 导致音频固定超前 skip_duration（约 0.5~1 秒）。
    // 修复：第一帧真正输出时（skip 完成后）才记录 MediaClock 作为 base，保证 A/V PTS 对齐。
    LOG_INFO("[CompositorEncoderBridge] Attached insert video source: " + source->get_id() +
             " (time base will be set on first frame output)");

    // 设置帧回调，解码后同时：1) 推送到画布渲染 2) 推入同步器用于编码
    // pts_ms 在外部时间基准未设置时为文件原始 PTS，设置后为流时间 PTS
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

            // 首帧：在 skip 完成后首次输出时设置时间基准
            // 此时 external_base 未设置 → pts_ms 为文件原始 PTS
            // 记录当前 MediaClock 和文件 PTS，保证后续帧 PTS 与 MediaClock 对齐
            if (!source->has_external_time_base()) {
                int64_t file_pts = pts_ms;  // 此时 pts_ms 为文件原始 PTS
                int64_t now_us = media_clock_.get_elapsed_time_us();
                source->set_external_time_base_with_first_pts(now_us, file_pts);
                // 首帧 PTS 取当前 MediaClock（相对偏移 = 0）
                pts_ms = now_us / 1000;
                LOG_INFO("[CompositorEncoderBridge] Insert video time base set on first frame:"
                         " file_pts=" + std::to_string(file_pts) +
                         "ms stream_base=" + std::to_string(now_us) + "us"
                         " -> stream_pts=" + std::to_string(pts_ms) + "ms");
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

// ---------------------------------------------------------------------------
// 确保 nv12_snapshot_ 存在且尺寸匹配（USAGE_DEFAULT, BindFlags=0）。
// 用于 GPU→GPU 快照拷贝（不走 STAGING，避免 Intel detile 崩溃）。
// ---------------------------------------------------------------------------
bool CompositorEncoderBridge::ensure_nv12_snapshot(int w, int h)
{
    if (nv12_snapshot_) {
        D3D11_TEXTURE2D_DESC existing{};
        nv12_snapshot_->GetDesc(&existing);
        if (static_cast<int>(existing.Width) == w && static_cast<int>(existing.Height) == h)
            return true;
        nv12_snapshot_ = nullptr;
    }

    auto& shared = SharedD3D11Device::instance();
    ID3D11Device* device = shared.device();
    if (!device) return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width          = static_cast<UINT>(w);
    desc.Height         = static_cast<UINT>(h);
    desc.MipLevels      = 1;
    desc.ArraySize      = 1;
    desc.Format         = DXGI_FORMAT_NV12;
    desc.SampleDesc     = { 1, 0 };
    desc.Usage          = D3D11_USAGE_DEFAULT;
    desc.BindFlags      = 0;   // 无绑定标志：纯 GPU 中间缓冲，避免 tiled 格式
    desc.CPUAccessFlags = 0;
    desc.MiscFlags      = 0;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, nv12_snapshot_.put());
    if (FAILED(hr)) {
        LOG_ERROR("[BRIDGE] ensure_nv12_snapshot: CreateTexture2D failed hr=0x" +
                  std::to_string(static_cast<uint32_t>(hr)));
        return false;
    }
    LOG_INFO("[BRIDGE] NV12 snapshot texture created: " + std::to_string(w) + "x" + std::to_string(h));
    return true;
}


} // namespace live_assistant
