#include "video_engine/video_capture_backend.h"
#include "video_engine/ffmpeg_capture_backend.h"
#include "video_engine/opencv_capture_backend.h"

namespace live_assistant {

// VideoCaptureBackendFactory实现
std::unique_ptr<VideoCaptureBackend> VideoCaptureBackendFactory::create_ffmpeg_backend() {
    return std::make_unique<FFmpegCaptureBackend>();
}

std::unique_ptr<VideoCaptureBackend> VideoCaptureBackendFactory::create_opencv_backend() {
    return std::make_unique<OpenCVCaptureBackend>();
}

std::unique_ptr<VideoCaptureBackend> VideoCaptureBackendFactory::create_backend(const std::string& backend_name) {
    if (backend_name == "FFmpeg" || backend_name == "ffmpeg") {
        return create_ffmpeg_backend();
    } else if (backend_name == "OpenCV" || backend_name == "opencv") {
        return create_opencv_backend();
    }
    return nullptr;
}

} // namespace live_assistant