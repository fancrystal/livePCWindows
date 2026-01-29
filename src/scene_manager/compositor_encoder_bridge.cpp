#include "scene_manager/compositor_encoder_bridge.h"
#include "common/log.h"
#include "common/error.h"
#include "video_engine/video_engine.h"
#include "audio_engine/audio_engine.h"

#include <QImage>
#include <QPainter>

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

namespace live_assistant {

CompositorEncoderBridge::CompositorEncoderBridge(QObject* parent)
    : QObject(parent), media_clock_() {
    encode_timer_ = new QTimer(this);
    connect(encode_timer_, &QTimer::timeout, this, &CompositorEncoderBridge::on_encode_timer);

    LOG_INFO("CompositorEncoderBridge created");
}

CompositorEncoderBridge::~CompositorEncoderBridge() {
    stop();
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

void CompositorEncoderBridge::set_encoder(std::shared_ptr<Encoder> encoder) {
    encoder_ = encoder;
    LOG_INFO("Encoder set for encoder bridge");
}

void CompositorEncoderBridge::set_stream_pusher(std::shared_ptr<StreamPusher> stream_pusher) {
    stream_pusher_ = stream_pusher;
    LOG_INFO("Stream pusher set for encoder bridge");
}

void CompositorEncoderBridge::set_audio_engine(std::shared_ptr<AudioEngine> audio_engine) {
    audio_engine_ = audio_engine;
    LOG_INFO("Audio engine set for encoder bridge");
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

    initialize_opengl_context();

    int interval = 1000 / fps_;
    encode_timer_->start(interval);

    LOG_INFO("CompositorEncoderBridge started with " + std::to_string(fps) + " fps");
}

void CompositorEncoderBridge::stop() {
    if (!running_) {
        return;
    }

    running_ = false;
    encode_timer_->stop();

    LOG_INFO("CompositorEncoderBridge stopped");
}

bool CompositorEncoderBridge::is_running() const {
    return running_;
}

bool CompositorEncoderBridge::start_streaming(const std::string& url) {
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
    // trim whitespace
    auto trim = [](std::string s) {
        const char* ws = " \t\n\r\f\v";
        size_t start = s.find_first_not_of(ws);
        if (start == std::string::npos) return std::string();
        size_t end = s.find_last_not_of(ws);
        return s.substr(start, end - start + 1);
    };
    std::string url_trim = trim(url);
    size_t last_slash = url_trim.find_last_of('/');
    if (last_slash != std::string::npos && last_slash > 7) {  // 确保在协议部分之后
        server_url = url_trim.substr(0, last_slash);
        stream_key = url_trim.substr(last_slash + 1);
    }

    config.server_url = server_url;
    config.stream_key = stream_key;
    config.protocol = StreamProtocol::RTMP;
    LOG_INFO("[BRIDGE] Parsed stream server_url: " + config.server_url + ", stream_key: " + config.stream_key);

    ErrorCode config_result = stream_pusher_->set_config(config);
    if (config_result != ErrorCode::SUCCESS) {
        LOG_ERROR("Failed to set stream config for: " + url);
        emit streaming_error(QString("Failed to set stream config: %1").arg(static_cast<int>(config_result)));
        return false;
    }
    // Ensure audio/video streams are registered before starting the pusher.
    if (encoder_) {
        AVCodecParameters* a_par = encoder_->get_audio_codec_parameters();
        AVRational a_tb = encoder_->get_audio_time_base();
        AVCodecParameters* v_par = encoder_->get_video_codec_parameters();
        AVRational v_tb = encoder_->get_video_time_base();

        // If video codec params are null, attempt to fallback to software encoder and reinitialize.
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

        if (!a_par || a_tb.den <= 0 || !v_par || v_tb.den <= 0) {
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

    // Request encoder to emit a keyframe for the next encoded frame before starting push
    if (encoder_) {
        encoder_->force_keyframe();
        // Reset audio encoder to ensure PTS starts from 0 for proper A/V sync
        encoder_->reset_audio_encoder();
    }

    ErrorCode start_result = stream_pusher_->start();
    if (start_result != ErrorCode::SUCCESS) {
        LOG_ERROR("Failed to start streaming to: " + url);
        emit streaming_error(QString("Failed to start streaming: %1").arg(static_cast<int>(start_result)));
        return false;
    }

    // Start media clock for timestamp synchronization
    media_clock_.start();

    streaming_ = true;
    stream_url_ = url;
    emit streaming_started();
    LOG_INFO("Streaming started to: " + url);
    return true;
}

void CompositorEncoderBridge::stop_streaming() {
    if (!streaming_) {
        return;
    }

    if (stream_pusher_) {
        stream_pusher_->stop();
    }

    // Stop media clock
    media_clock_.stop();

    streaming_ = false;
    stream_url_.clear();
    emit streaming_stopped();
    LOG_INFO("Streaming stopped");
}

bool CompositorEncoderBridge::is_streaming() const {
    return streaming_;
}

void CompositorEncoderBridge::set_fps(int fps) {
    fps_ = fps;
    if (running_) {
        int interval = 1000 / fps_;
        encode_timer_->start(interval);
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
            original_fps_ = fps_;
            fps_ = degraded_fps_;
            encode_timer_->setInterval(1000 / fps_);
            quality_degraded_ = true;

            emit quality_degraded(QString("FPS dropped to %1, reducing to %2 fps").arg(avg_fps).arg(fps_));
            LOG_WARNING("Quality degraded due to low FPS: " + std::to_string(avg_fps));

        } else if (quality_degraded_ && avg_fps > max_fps_threshold_) {
            fps_ = original_fps_;
            encode_timer_->setInterval(1000 / fps_);
            quality_degraded_ = false;

            emit quality_restored();
            LOG_INFO("Quality restored, FPS back to: " + std::to_string(avg_fps));
        }
    }

    encode_and_push_frame();
}

void CompositorEncoderBridge::encode_and_push_frame() {
    try {
        // 获取从推流开始算起的相对时间戳
        auto current_timestamp_us = media_clock_.get_elapsed_time_us();
        auto current_timestamp_ms = current_timestamp_us / 1000;

        // 编码视频帧
        auto video_frame = capture_compositor_frame();
        if (video_frame) {
            video_frame->timestamp = MediaTimestamp(current_timestamp_us);
            video_frame->timestamp_ms = current_timestamp_ms;

            std::vector<EncodedPacketPtr> video_packets;
            ErrorCode video_result = encoder_->encode_video_frame(video_frame, video_packets);

            if (video_result == ErrorCode::SUCCESS && !video_packets.empty() && stream_pusher_ && stream_pusher_->is_pushing()) {
                for (auto& p : video_packets) {
                    if (!p) continue;
                    p->wallclock_us = current_timestamp_us;
                    stream_pusher_->push_packet(p);
                }
            }
        }

        // 编码音频帧（优先使用音频引擎），若无可用帧则自动合成静音帧以保持A/V同步
        std::shared_ptr<AudioFrame> audio_frame = nullptr;
        bool using_real_audio = false;

        if (audio_engine_) {
            audio_frame = audio_engine_->get_audio_frame();
            if (audio_frame) {
                using_real_audio = true;
            }
        }

        if (!audio_frame) {
            // 自动生成静音帧以保持A/V同步
            int sample_rate = 44100;
            int channels = 2;
            if (audio_engine_) {
                sample_rate = audio_engine_->get_sample_rate();
                channels = audio_engine_->get_channels();
            } else if (encoder_) {
                auto acfg = encoder_->get_audio_config();
                if (acfg.sample_rate > 0) sample_rate = acfg.sample_rate;
                if (acfg.channels > 0) channels = acfg.channels;
            }
            int samples = 2048;
            audio_frame = std::make_shared<AudioFrame>(sample_rate, channels, samples);
        }

        if (audio_frame) {
            audio_frame->timestamp = MediaTimestamp(current_timestamp_us);
            audio_frame->timestamp_ms = current_timestamp_ms;

            std::vector<EncodedPacketPtr> audio_packets;
            ErrorCode audio_result = encoder_->encode_audio_frame(audio_frame, audio_packets);

            if (audio_result == ErrorCode::SUCCESS && !audio_packets.empty() && stream_pusher_ && stream_pusher_->is_pushing()) {
                for (auto& p : audio_packets) {
                    if (!p) continue;
                    p->wallclock_us = current_timestamp_us;
                    stream_pusher_->push_packet(p);
                }
            }
        }

    } catch (const std::exception& ex) {
        LOG_ERROR("Exception in encode_and_push_frame: " + std::string(ex.what()));
    }
}

std::shared_ptr<VideoFrame> CompositorEncoderBridge::capture_compositor_frame() {
    if (!compositor_) {
        return nullptr;
    }

    const QImage img = compositor_->render_to_image(width_, height_).convertToFormat(QImage::Format_RGBA8888);
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

    SwsContext* sws = sws_getContext(
        static_cast<int>(width_), static_cast<int>(height_), AV_PIX_FMT_RGBA,
        static_cast<int>(width_), static_cast<int>(height_), AV_PIX_FMT_NV12,
        SWS_BILINEAR,
        nullptr, nullptr, nullptr);

    if (!sws) {
        return nullptr;
    }

    const uint8_t* src_slices[1] = { reinterpret_cast<const uint8_t*>(img.constBits()) };
    int src_strides[1] = { static_cast<int>(img.bytesPerLine()) };

    uint8_t* dst_slices[2] = { frame->data.get(), frame->data_uv.get() };
    int dst_strides[2] = { frame->stride, frame->stride_uv };

    sws_scale(sws, src_slices, src_strides, 0, height_, dst_slices, dst_strides);
    sws_freeContext(sws);

    return frame;
}

void CompositorEncoderBridge::initialize_opengl_context() {
    LOG_INFO("OpenGL context initialization skipped for encoder bridge");
}

} // namespace live_assistant
