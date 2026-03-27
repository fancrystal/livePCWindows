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
    if (gpu_texture_mode_) {
        // GPU 路径：纹理直传，跳过 CPU 回读
        loop_->set_texture_callback([this](const GpuTextureRef& ref) { on_texture(ref); });
    } else {
        // CPU 路径（默认，向后兼容）
        loop_->set_frame_callback([this](const QImage& img) { on_image(img); });
    }
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

void WGCaptureSourceAdapter::enable_gpu_texture_mode()
{
    if (!loop_) initialize();
    gpu_texture_mode_ = true;
    if (loop_) {
        loop_->set_texture_callback([this](const GpuTextureRef& ref) { on_texture(ref); });
    }
    LOG_INFO("[WGC] GPU texture mode enabled for source: " + cfg_.target_id);
}

void WGCaptureSourceAdapter::on_texture(const GpuTextureRef& tex_ref)
{
    if (!tex_ref.is_valid()) return;
    emit textureReady(tex_ref, QString::fromStdString(cfg_.target_id));
}

void WGCaptureSourceAdapter::update_share_settings(bool capture_cursor, bool capture_border) {
    if (loop_) {
        loop_->update_settings(capture_cursor, capture_border);
        cfg_.capture_cursor = capture_cursor;
        cfg_.capture_border = capture_border;
        LOG_INFO("WGCaptureSourceAdapter: Updated share settings - cursor=" +
                 std::string(capture_cursor ? "true" : "false") +
                 ", border=" + std::string(capture_border ? "true" : "false"));
    }
}

} // namespace live_assistant
