#include "scene_manager/wgc_capture_source.h"
#include "scene_manager/wgc_capture_loop.h"
#include "common/media_clock.h"
#include "common/log.h"

namespace live_assistant {

WGCaptureSourceAdapter::WGCaptureSourceAdapter(const CaptureConfig& cfg) : cfg_(cfg) {}

WGCaptureSourceAdapter::~WGCaptureSourceAdapter() { shutdown(); }

bool WGCaptureSourceAdapter::initialize() {
    if (loop_) return true;
    loop_ = std::make_unique<WGCCaptureLoop>(cfg_);
    loop_->set_frame_callback([this](const QImage& img) { on_image(img); });
    return true;
}

bool WGCaptureSourceAdapter::start() {
    if (running_) return true;
    if (!loop_ && !initialize()) return false;
    if (!loop_->start()) return false;
    running_ = true;
    return true;
}

bool WGCaptureSourceAdapter::stop() {
    if (!running_) return true;
    if (loop_) loop_->stop();
    running_ = false;
        return true;
}

bool WGCaptureSourceAdapter::shutdown() {
    stop();
    loop_.reset();
    return true;
}

bool WGCaptureSourceAdapter::is_running() const { return running_; }

void WGCaptureSourceAdapter::on_image(const QImage& img) {
    // 每帧都打印会导致 UI 卡顿，仅在调试时打开
    LOG_DEBUG("[DIAG] WGCaptureSourceAdapter::on_image - 收到图像，isNull: " + std::string(img.isNull() ? "true" : "false") +
             ", 尺寸: " + std::to_string(img.width()) + "x" + std::to_string(img.height()));

    if (img.isNull()) return;

    CaptureFrame frame;
    frame.image = img;
    frame.width = img.width();
    frame.height = img.height();
    frame.timestamp = MediaClock().now();

    LOG_DEBUG("[DIAG] WGCaptureSourceAdapter::on_image - 发送frameReady信号，源ID: " + cfg_.target_id);
    emit frameReady(frame);
}

} // namespace live_assistant
