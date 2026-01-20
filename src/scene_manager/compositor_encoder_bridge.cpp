#include "scene_manager/compositor_encoder_bridge.h"
#include "common/log.h"
#include "common/error.h"
#include "video_engine/video_engine.h"
#include <chrono>

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
        auto video_frame = capture_compositor_frame();
        if (!video_frame) {
            LOG_WARNING("Failed to capture frame from compositor");
            return;
        }

        video_frame->timestamp = media_clock_.now();
        video_frame->timestamp_ms = video_frame->timestamp.us / 1000;

        std::vector<EncodedPacketPtr> packets;
        ErrorCode result = encoder_->encode_video_frame(video_frame, packets);

        if (result != ErrorCode::SUCCESS || packets.empty()) {
            LOG_WARNING("Failed to encode video frame");
            return;
        }

        if (stream_pusher_ && stream_pusher_->is_pushing()) {
            for (auto& p : packets) {
                if (!p) continue;
                p->wallclock_us = video_frame->timestamp.us;
                stream_pusher_->push_packet(p);
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
        width_, height_, AV_PIX_FMT_RGBA,
        width_, height_, AV_PIX_FMT_NV12,
        SWS_BILINEAR,
        nullptr, nullptr, nullptr);

    if (!sws) {
        return nullptr;
    }

    const uint8_t* src_slices[1] = { img.constBits() };
    int src_strides[1] = { img.bytesPerLine() };

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
