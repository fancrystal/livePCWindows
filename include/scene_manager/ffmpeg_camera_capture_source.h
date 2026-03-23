#pragma once

#include "scene_manager/icapture_source.h"

#include <atomic>
#include <thread>

namespace live_assistant {

class FFmpegCameraCaptureSource : public ICaptureSource {
    Q_OBJECT
public:
    explicit FFmpegCameraCaptureSource(const CaptureConfig& config);
    ~FFmpegCameraCaptureSource() override;

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

    // FFmpeg resources
    void* fmt_ctx_{nullptr};
    void* codec_ctx_{nullptr};
    void* sws_ctx_{nullptr};
    int video_stream_index_{-1};
};

} // namespace live_assistant
