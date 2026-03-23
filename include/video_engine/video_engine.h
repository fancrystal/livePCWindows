#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>

#include "common/error.h"
#include "common/media_clock.h"
#include "scene_manager/icapture_source.h"  // 使用统一的 CaptureMode 枚举

namespace live_assistant {

// Forward declarations
class Scene;
struct VideoFrame;

// 摄像头设备信息结构体
typedef struct {
    std::string name;       // 设备路径
    std::string description; // 友好名称
} CameraDevice;

// VideoEngine配置结构体
struct VideoEngineConfig {
    int width = 1920;
    int height = 1080;
    int fps = 30;
    std::string camera_id;
    std::string camera_resolution = "640x360";
    int camera_fps = 30;
    std::string camera_pixel_format = "PIXEL_FORMAT_YUY2";
    bool camera_mirror = false;
};

class VideoEngine {
public:
    // 使用 live_assistant::CaptureMode 枚举（在 icapture_source.h 中定义）
    // 不再定义自己的 CaptureMode 枚举

    struct CameraChoice {
        std::string display_name;   // UI display
        std::string dshow_name;     // FFmpeg dshow device_name (Friendly Name)
        int opencv_index = -1;      // OpenCV camera index
    };

    VideoEngine();
    ~VideoEngine();

    bool initialize(int width, int height, int fps);
    bool shutdown();

    bool start_capture();
    bool stop_capture();

    // 获取可用视频设备（显示名）
    std::vector<std::string> get_available_cameras();

    // 获取可用摄像头（显示名 + dshow device_name）
    std::vector<CameraChoice> get_available_camera_choices();

    bool select_camera(const std::string& camera_id);

    // 设置捕获模式（FFMPEG、OPENCV）
    bool set_capture_mode(CaptureMode mode);
    CaptureMode get_capture_mode() const;
    
    // 设置摄像头参数
    bool set_camera_resolution(const std::string& resolution);
    bool set_camera_fps(int fps);
    bool set_camera_pixel_format(const std::string& pixel_format);
    bool set_camera_mirror(bool mirror);
    
    // 设置要渲染的场景
    void set_current_scene(std::shared_ptr<Scene> scene);
    
    // 获取最新捕获的帧（线程安全）
    std::shared_ptr<VideoFrame> get_latest_frame();
    
    // 从当前场景渲染一帧
    std::shared_ptr<VideoFrame> render_frame();
    
    // 获取当前输出分辨率
    int get_output_width() const;
    int get_output_height() const;
    
    // 获取摄像头参数
    std::string get_camera_resolution() const;
    int get_camera_fps() const;
    std::string get_camera_pixel_format() const;
    bool get_camera_mirror() const;
    
private:
    bool initialize_ffmpeg();
    void release_ffmpeg();
    bool initialize_opencv();
    void release_opencv();
    
    void capture_thread_func();
    void ffmpeg_capture_thread_func();
    void opencv_capture_thread_func();
    
    int output_width_ = 1920;
    int output_height_ = 1080;
    int fps_ = 30;
    
    std::string camera_resolution_ = "640x360";
    int camera_fps_ = 30;
    std::string camera_pixel_format_ = "PIXEL_FORMAT_YUY2";
    bool camera_mirror_ = false;
    
    std::shared_ptr<Scene> current_scene_;
    bool is_capturing_ = false;
    std::string selected_camera_;
    CaptureMode capture_mode_ = CaptureMode::FFMPEG;
    
    void* av_format_context_ = nullptr;
    void* av_codec_context_ = nullptr;
    void* sws_context_ = nullptr;
    int video_stream_index_ = -1;
    
    void* cv_video_capture_ = nullptr;
    
    std::thread capture_thread_;
    bool stop_thread_ = false;
    
    std::shared_ptr<VideoFrame> latest_frame_;
    std::mutex frame_mutex_;
    std::condition_variable frame_cv_;
    
    bool ffmpeg_initialized_ = false;
    bool opencv_initialized_ = false;

    std::vector<CameraDevice> camera_devices_;
};

struct VideoFrame {
    enum class PixelFormat {
        RGBA,
        NV12,
    };

    PixelFormat format = PixelFormat::RGBA;

    std::unique_ptr<uint8_t[]> data;
    std::unique_ptr<uint8_t[]> data_uv;

    int width = 0;
    int height = 0;

    int stride = 0;
    int stride_uv = 0;

    // 🔧 统一使用毫秒时间戳 (timestamp_ms)
    // 之前有 timestamp (微秒) 和 timestamp_ms (毫秒) 两个字段，容易混淆
    int64_t timestamp_ms = 0;

    VideoFrame() = default;
    VideoFrame(int w, int h);

    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;

    VideoFrame(VideoFrame&& other) noexcept = default;
    VideoFrame& operator=(VideoFrame&& other) noexcept = default;
};

} // namespace live_assistant
