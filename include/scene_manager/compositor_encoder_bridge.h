#pragma once

#include <memory>
#include <QTimer>
#include <QObject>
#include <QOpenGLContext>
#include <QOffscreenSurface>

#include "scene_manager/compositor.h"
#include "encoder/encoder.h"
#include "stream_pusher/stream_pusher.h"
#include "common/media_clock.h"

namespace live_assistant {

class CompositorEncoderBridge : public QObject {
    Q_OBJECT

public:
    explicit CompositorEncoderBridge(QObject* parent = nullptr);
    ~CompositorEncoderBridge() override;

    // 设置组件
    void set_compositor(std::shared_ptr<Compositor> compositor);
    void set_encoder(std::shared_ptr<Encoder> encoder);
    void set_stream_pusher(std::shared_ptr<StreamPusher> stream_pusher);

    // 控制
    void start(int fps = 30);
    void stop();
    bool is_running() const;

    // 配置
    void set_fps(int fps);
    void set_resolution(int width, int height);

    // 降级策略
    void enable_adaptive_quality(bool enable);
    void set_quality_thresholds(int min_fps, int max_fps);

signals:
    void frame_encoded(const std::vector<uint8_t>& data);
    void quality_degraded(const QString& reason);
    void quality_restored();

private slots:
    void on_compositor_frame_ready();
    void on_encode_timer();

private:
    void encode_and_push_frame();
    std::shared_ptr<VideoFrame> capture_compositor_frame();
    void initialize_opengl_context();

    // 组件
    std::shared_ptr<Compositor> compositor_;
    std::shared_ptr<Encoder> encoder_;
    std::shared_ptr<StreamPusher> stream_pusher_;


    // 定时器
    QTimer* encode_timer_;

    // 配置
    int fps_ = 30;
    int width_ = 1920;
    int height_ = 1080;

    // 状态
    bool running_ = false;
    MediaClock media_clock_;

    // 降级策略
    bool adaptive_quality_enabled_ = true;
    int min_fps_threshold_ = 20;
    int max_fps_threshold_ = 25;
    bool quality_degraded_ = false;
    int original_fps_ = 30;
    int degraded_fps_ = 15;
};

} // namespace live_assistant