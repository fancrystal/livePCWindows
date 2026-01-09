#include "scene_manager/compositor_encoder_bridge.h"
#include "common/log.h"
#include "common/error.h"
#include "video_engine/video_engine.h"
#include <chrono>

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

    // Initialize OpenGL context for offscreen rendering
    initialize_opengl_context();

    // Start encode timer
    int interval = 1000 / fps_;  // milliseconds
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
    // This is called when compositor has rendered a new frame
    // We don't need to do anything here as we use timer-based encoding
}

void CompositorEncoderBridge::on_encode_timer() {
    if (!running_) {
        return;
    }

    // Adaptive quality check
    if (adaptive_quality_enabled_ && compositor_) {
        double avg_fps, avg_render_time;
        size_t frame_count;
        compositor_->get_performance_stats(avg_fps, avg_render_time, frame_count);

        if (!quality_degraded_ && avg_fps < min_fps_threshold_ && avg_fps > 0) {
            // Degrade quality
            original_fps_ = fps_;
            fps_ = degraded_fps_;
            encode_timer_->setInterval(1000 / fps_);
            quality_degraded_ = true;

            emit quality_degraded(QString("FPS dropped to %1, reducing to %2 fps").arg(avg_fps).arg(fps_));
            LOG_WARNING("Quality degraded due to low FPS: " + std::to_string(avg_fps));

        } else if (quality_degraded_ && avg_fps > max_fps_threshold_) {
            // Restore quality
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
        // Capture frame from compositor
        auto video_frame = capture_compositor_frame();
        if (!video_frame) {
            LOG_WARNING("Failed to capture frame from compositor");
            return;
        }

        // Set timestamp
        video_frame->timestamp = media_clock_.now();
        video_frame->timestamp_ms = video_frame->timestamp.us / 1000;

        // Encode video frame
        std::vector<uint8_t> encoded_data;
        ErrorCode result = encoder_->encode_video_frame(video_frame, encoded_data);

        if (result != ErrorCode::SUCCESS || encoded_data.empty()) {
            LOG_WARNING("Failed to encode video frame");
            return;
        }

        // Create media packet
        MediaPacket packet;
        packet.type = MediaType::VIDEO;
        packet.timestamp = video_frame->timestamp.us;
        packet.data = std::move(encoded_data);
        packet.is_keyframe = (rand() % 30 == 0); // Simulate keyframe every ~1 second at 30fps
        packet.is_config = false;

        // Set video frame type
        if (packet.is_keyframe) {
            packet.video_type = VideoFrameType::I_FRAME;
        } else {
            packet.video_type = VideoFrameType::P_FRAME;
        }

        packet.priority = 1; // Video priority lower than audio

        // Push to stream
        if (stream_pusher_ && stream_pusher_->is_pushing()) {
            stream_pusher_->push_packet(packet);
        }

        // Emit signal for external monitoring
        emit frame_encoded(packet.data);

        LOG_DEBUG("Encoded and pushed video frame, size: " + std::to_string(packet.data.size()));

    } catch (const std::exception& ex) {
        LOG_ERROR("Exception in encode_and_push_frame: " + std::string(ex.what()));
    }
}

std::shared_ptr<VideoFrame> CompositorEncoderBridge::capture_compositor_frame() {
    if (!compositor_) {
        return nullptr;
    }

    // Create video frame with test pattern
    auto frame = std::make_shared<VideoFrame>(width_, height_);

    // Fill with a simple test pattern (gradient)
    uint8_t* data = frame->data.get();
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            int index = (y * width_ + x) * 4;  // RGBA
            data[index + 0] = (x * 255) / width_;      // R
            data[index + 1] = (y * 255) / height_;     // G
            data[index + 2] = 128;                     // B
            data[index + 3] = 255;                     // A
        }
    }

    // Set stride (4 bytes per pixel for RGBA)
    frame->stride = width_ * 4;

    return frame;
}

void CompositorEncoderBridge::initialize_opengl_context() {
    // No OpenGL context needed for simple implementation
    LOG_INFO("OpenGL context initialization skipped for encoder bridge");
}

} // namespace live_assistant