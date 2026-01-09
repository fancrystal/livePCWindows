#pragma once

#include "scene_manager/icapture_source.h"
#include <atomic>
#include <thread>
#include <mutex>

namespace live_assistant {

class PrintWindowCaptureSource : public ICaptureSource {
public:
    explicit PrintWindowCaptureSource(const CaptureConfig& config);
    ~PrintWindowCaptureSource() override;

    bool initialize() override;
    bool start() override;
    bool stop() override;
    bool shutdown() override;

    void set_frame_callback(CaptureFrameCallback cb) override;
    const CaptureConfig& get_config() const override { return config_; }

private:
    CaptureConfig config_;
    CaptureFrameCallback frame_cb_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex cb_mutex_;

    void worker_loop();
};

} // namespace live_assistant

