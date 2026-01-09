#pragma once

#include "video_engine/video_capture_backend.h"
#include <memory>

namespace live_assistant {

class OpenCVCaptureBackend : public VideoCaptureBackend {
public:
    OpenCVCaptureBackend();
    ~OpenCVCaptureBackend() override;

    ErrorCode initialize(int width, int height, int fps);
    ErrorCode start();
    ErrorCode stop();
    ErrorCode shutdown();

    std::vector<std::string> get_available_devices() override;
    ErrorCode select_device(const std::string& device_id) override;
    ErrorCode capture_frame(std::shared_ptr<VideoFrame>& frame) override;
    bool is_running() const override;
    const char* get_backend_name() const override { return "OpenCV"; }

private:
    // OpenCV相关成员（使用void*隐藏实现细节）
    void* cv_video_capture_ = nullptr;

    // 配置参数
    int output_width_ = 1920;
    int output_height_ = 1080;
    int fps_ = 30;
    std::string selected_device_;

    bool is_initialized_ = false;
    bool is_capturing_ = false;

    // 内部辅助方法
    bool initialize_opencv();
    void release_opencv();
};

} // namespace live_assistant