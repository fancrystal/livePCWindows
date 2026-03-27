#pragma once

#include "scene_manager/icapture_source.h"
#include "scene_manager/wgc_capture_loop.h"
#include "scene_manager/gpu_texture_ref.h"
#include <memory>
#include <QImage>
#include <atomic>


namespace live_assistant {

// Adapter that wraps existing CaptureSource implementation to ICaptureSource.
class WGCaptureSourceAdapter : public ICaptureSource {
    Q_OBJECT
public:
    explicit WGCaptureSourceAdapter(const CaptureConfig& cfg);
    ~WGCaptureSourceAdapter() override;

    bool initialize() override;
    bool start() override;
    bool stop() override;
    bool shutdown() override;

    const CaptureConfig& get_config() const override { return cfg_; }
    bool is_running() const override;

    // 更新共享设置（运行时生效）
    void update_share_settings(bool capture_cursor, bool capture_border);

    // Phase 1: 启用 GPU 纹理直传模式
    // 调用后，每帧通过 textureReady 信号发出 GpuTextureRef，
    // 同时禁用 CPU 回读（frameReady 不再发出）
    void enable_gpu_texture_mode();

signals:
    // Phase 1: GPU 纹理直传信号（与 frameReady 互斥，enable_gpu_texture_mode 后生效）
    void textureReady(const live_assistant::GpuTextureRef& tex_ref, const QString& source_id);

private:
    void on_image(const QImage& img);
    void on_texture(const GpuTextureRef& tex_ref);   // Phase 1

    CaptureConfig cfg_;
    std::unique_ptr<WGCCaptureLoop> loop_;

    std::atomic<bool> running_{false};
    bool gpu_texture_mode_ = false;   // Phase 1
};

} // namespace live_assistant

