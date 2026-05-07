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
    // 注册初始化失败回调，将 HRESULT 转换为用户友好提示并发出 captureError 信号
    loop_->set_error_callback([this](HRESULT hr) {
        on_capture_error(hr);
    });
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

void WGCaptureSourceAdapter::on_capture_error(HRESULT hr)
{
    QString msg;
    // E_ACCESSDENIED (0x80070005): 显示器被内容保护，WGC 被 DWM 拒绝
    // 常见原因：360安全卫士、联想管家、企业DLP、银行APP等调用了
    //           SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)
    if (hr == static_cast<HRESULT>(0x80070005)) {
        msg = QString("屏幕共享初始化失败：当前显示器受到内容保护（错误 E_ACCESSDENIED）。\n\n"
                      "可能原因：360安全卫士、联想管家、杀毒软件或银行类APP在该显示器上设置了截图保护。\n\n"
                      "解决方法：关闭上述软件后，重新添加共享屏幕。");
    } else {
        char hrBuf[32];
        snprintf(hrBuf, sizeof(hrBuf), "0x%08X", static_cast<unsigned>(hr));
        msg = QString("屏幕共享初始化失败（错误代码 %1）。\n\n"
                      "请检查显示器连接是否正常，或重启应用后重试。").arg(hrBuf);
    }

    LOG_ERROR("WGCaptureSourceAdapter: captureError emitted for target=" +
              cfg_.target_id + ", hr=0x" +
              [hr]() {
                  char buf[16];
                  snprintf(buf, sizeof(buf), "%08X", static_cast<unsigned>(hr));
                  return std::string(buf);
              }());

    // 从工作线程调用，通过信号跨线程投递到主线程（Qt::QueuedConnection 自动处理）
    emit captureError(QString::fromStdString(cfg_.target_id), msg);
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
