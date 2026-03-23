#pragma once

#include "scene_manager/icapture_source.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace live_assistant {

class OpenCVCameraCaptureSource : public ICaptureSource {
    Q_OBJECT
public:
    explicit OpenCVCameraCaptureSource(const CaptureConfig& config);
    ~OpenCVCameraCaptureSource() override;

    bool initialize() override;
    bool start() override;
    bool stop() override;
    bool shutdown() override;

    const CaptureConfig& get_config() const override { return config_; }
    bool is_running() const override { return running_.load(); }

private:
    void capture_loop();

    CaptureConfig config_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> initialized_{false};

    std::thread th_;

    // Pimpl-like holder to avoid including OpenCV headers in public interface
    void* cap_{nullptr};
};

} // namespace live_assistant
