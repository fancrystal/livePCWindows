#pragma once

#include "scene_manager/icapture_source.h"
#include <atomic>
#include <thread>

namespace live_assistant {

class PrintWindowCaptureSource : public ICaptureSource {
    Q_OBJECT
public:
    explicit PrintWindowCaptureSource(const CaptureConfig& config);
    ~PrintWindowCaptureSource() override;

    bool initialize() override;
    bool start() override;
    bool stop() override;
    bool shutdown() override;

    const CaptureConfig& get_config() const override { return config_; }
    bool is_running() const override;

private:
    void worker_loop();
    void cleanup_dib();

    CaptureConfig config_;
    std::atomic<bool> running_{false};
    std::thread worker_;

    // Cached DIB resources
    int cached_width_ = 0;
    int cached_height_ = 0;
    void* cached_bits_ = nullptr;
    HDC cached_hdc_mem_ = nullptr;
    HBITMAP cached_hbitmap_ = nullptr;
    HBITMAP cached_old_bitmap_ = nullptr;
    HDC cached_hdc_window_ = nullptr;
};

} // namespace live_assistant

