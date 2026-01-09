#pragma once

#include "video_engine/video_capture_backend.h"
#include <memory>

namespace live_assistant {

class FFmpegCaptureBackend : public VideoCaptureBackend {
public:
    FFmpegCaptureBackend();
    ~FFmpegCaptureBackend() override;

    ErrorCode initialize(int width, int height, int fps);
    ErrorCode start();
    ErrorCode stop();
    ErrorCode shutdown();

    std::vector<std::string> get_available_devices() override;
    ErrorCode select_device(const std::string& device_id) override;
    ErrorCode capture_frame(std::shared_ptr<VideoFrame>& frame) override;
    bool is_running() const override;
    const char* get_backend_name() const override { return "FFmpeg"; }

private:
    // FFmpeg相关成员（使用void*隐藏实现细节）
    void* av_format_context_ = nullptr;
    void* av_codec_context_ = nullptr;
    void* sws_context_ = nullptr;
    int video_stream_index_ = -1;

    // 配置参数
    int output_width_ = 1920;
    int output_height_ = 1080;
    int fps_ = 30;
    std::string selected_device_;

    bool is_initialized_ = false;
    bool is_capturing_ = false;

    // 内部辅助方法
    bool initialize_ffmpeg();
    void release_ffmpeg();
};

} // namespace live_assistant