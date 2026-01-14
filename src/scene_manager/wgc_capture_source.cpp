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

void WGCaptureSourceAdapter::set_frame_callback(CaptureFrameCallback cb) { frame_cb_ = std::move(cb); }

bool WGCaptureSourceAdapter::is_running() const { return running_; }

void WGCaptureSourceAdapter::on_image(const QImage& img) {
    if (img.isNull()) return;
    CaptureFrame frame;
    frame.image = img;
    frame.width = img.width();
    frame.height = img.height();
    frame.timestamp = MediaClock().now();
    CaptureFrameCallback cb = frame_cb_;
    if (cb) cb(frame);
}

} // namespace live_assistant
