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

    CaptureConfig config_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

} // namespace live_assistant

