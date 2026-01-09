#include "video_engine/ffmpeg_capture_backend.h"
#include "video_engine/video_engine.h"
#include "common/log.h"

namespace live_assistant {

FFmpegCaptureBackend::FFmpegCaptureBackend()
    : av_format_context_(nullptr),
      av_codec_context_(nullptr),
      sws_context_(nullptr),
      video_stream_index_(-1) {
}

FFmpegCaptureBackend::~FFmpegCaptureBackend() {
    shutdown();
}

ErrorCode FFmpegCaptureBackend::initialize(int width, int height, int fps) {
    output_width_ = width;
    output_height_ = height;
    fps_ = fps;
    is_initialized_ = true;
    LOG_INFO("FFmpeg capture backend initialized");
    return ErrorCode::SUCCESS;
}

ErrorCode FFmpegCaptureBackend::start() {
    if (!is_initialized_) {
        return ErrorCode::INVALID_STATE;
    }
    is_capturing_ = true;
    LOG_INFO("FFmpeg capture backend started");
    return ErrorCode::SUCCESS;
}

ErrorCode FFmpegCaptureBackend::stop() {
    is_capturing_ = false;
    LOG_INFO("FFmpeg capture backend stopped");
    return ErrorCode::SUCCESS;
}

ErrorCode FFmpegCaptureBackend::shutdown() {
    stop();
    release_ffmpeg();
    is_initialized_ = false;
    LOG_INFO("FFmpeg capture backend shutdown");
    return ErrorCode::SUCCESS;
}

std::vector<std::string> FFmpegCaptureBackend::get_available_devices() {
    // TODO: 实现真正的FFmpeg设备枚举
    return {"device1", "device2"};
}

ErrorCode FFmpegCaptureBackend::select_device(const std::string& device_id) {
    selected_device_ = device_id;
    LOG_INFO("Selected FFmpeg device: " + device_id);
    return ErrorCode::SUCCESS;
}

ErrorCode FFmpegCaptureBackend::capture_frame(std::shared_ptr<VideoFrame>& frame) {
    if (!is_capturing_) {
        return ErrorCode::INVALID_STATE;
    }

    // TODO: 实现真正的FFmpeg帧捕获
    // 这里暂时创建一个空的帧用于测试
    frame = std::shared_ptr<VideoFrame>(new VideoFrame(output_width_, output_height_));
    return ErrorCode::SUCCESS;
}

bool FFmpegCaptureBackend::is_running() const {
    return is_capturing_;
}

bool FFmpegCaptureBackend::initialize_ffmpeg() {
    // TODO: 实现FFmpeg初始化
    return true;
}

void FFmpegCaptureBackend::release_ffmpeg() {
    // TODO: 实现FFmpeg资源释放
}

} // namespace live_assistant