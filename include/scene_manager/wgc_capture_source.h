#pragma once

#include "scene_manager/icapture_source.h"
#include "scene_manager/wgc_capture_loop.h"
#include <memory>
#include <QImage>
#include <atomic>


namespace live_assistant {

// Adapter that wraps existing CaptureSource implementation to ICaptureSource.
class WGCaptureSourceAdapter : public ICaptureSource {
public:
    explicit WGCaptureSourceAdapter(const CaptureConfig& cfg);
    ~WGCaptureSourceAdapter() override;

    bool initialize() override;
    bool start() override;
    bool stop() override;
    bool shutdown() override;

    void set_frame_callback(CaptureFrameCallback cb) override;
    const CaptureConfig& get_config() const override { return cfg_; }
    bool is_running() const override;

private:
    void on_image(const QImage& img);

    CaptureConfig cfg_;
    std::unique_ptr<WGCCaptureLoop> loop_;

    CaptureFrameCallback frame_cb_;
    std::atomic<bool> running_{false};
};

} // namespace live_assistant

