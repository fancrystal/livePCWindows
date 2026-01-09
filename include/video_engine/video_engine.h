#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>

#include "common/error.h"
#include "common/media_clock.h"



namespace live_assistant {

// 前向声明
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
};

class VideoEngine {
public:
    enum class CaptureMode {
        FFMPEG,
        OPENCV
    };
    
    VideoEngine();
    ~VideoEngine();

    bool initialize(int width, int height, int fps);
    bool shutdown();

    bool start_capture();
    bool stop_capture();
    
    // 获取可用视频设备
    std::vector<std::string> get_available_cameras();
    bool select_camera(const std::string& camera_id);
    

    // 设置捕获模式（FFMPEG、OPENCV）
    bool set_capture_mode(CaptureMode mode);
    CaptureMode get_capture_mode() const;
    
    // 设置摄像头参数
    bool set_camera_resolution(const std::string& resolution);
    bool set_camera_fps(int fps);
    bool set_camera_pixel_format(const std::string& pixel_format);
    
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
    
private:
    // 初始化FFmpeg用于摄像头捕获
    bool initialize_ffmpeg();
    
    // 释放FFmpeg资源
    void release_ffmpeg();
    
    // 初始化OpenCV用于摄像头捕获
    bool initialize_opencv();
    
    // 释放OpenCV资源
    void release_opencv();
    
    // 捕获线程函数
    void capture_thread_func();
    
    // 使用FFmpeg的捕获线程函数
    void ffmpeg_capture_thread_func();
    
    // 使用OpenCV的捕获线程函数
    void opencv_capture_thread_func();
    

    
    int output_width_ = 1920;
    int output_height_ = 1080;
    int fps_ = 30;
    
    // 摄像头参数
    std::string camera_resolution_ = "640x360";
    int camera_fps_ = 30;
    std::string camera_pixel_format_ = "PIXEL_FORMAT_YUY2";
    
    std::shared_ptr<Scene> current_scene_;
    bool is_capturing_ = false;
    std::string selected_camera_;
    CaptureMode capture_mode_ = CaptureMode::FFMPEG;
    
    // FFmpeg相关成员（使用void*隐藏头文件中的FFmpeg类型）
    void* av_format_context_ = nullptr;
    void* av_codec_context_ = nullptr;
    void* sws_context_ = nullptr;
    int video_stream_index_ = -1;
    
    // OpenCV相关成员（使用void*隐藏头文件中的OpenCV类型）
    void* cv_video_capture_ = nullptr;
    

    
    // 线程管理
    std::thread capture_thread_;
    bool stop_thread_ = false;
    
    // 带线程安全的帧存储
    std::shared_ptr<VideoFrame> latest_frame_;
    std::mutex frame_mutex_;
    std::condition_variable frame_cv_;
    
    // 初始化标志
    bool ffmpeg_initialized_ = false;
    bool opencv_initialized_ = false;

    
    // 摄像头设备列表，存储设备路径和友好名称的映射
    std::vector<CameraDevice> camera_devices_;
};

// 简单的视频帧结构
struct VideoFrame {
    std::unique_ptr<uint8_t[]> data;
    int width = 0;
    int height = 0;
    int stride = 0;

    // 媒体时间戳（微秒）
    MediaTimestamp timestamp;

    // 遗留的毫秒时间戳（用于向后兼容）
    int64_t timestamp_ms = 0;

    VideoFrame() = default;
    VideoFrame(int w, int h);

    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;

    VideoFrame(VideoFrame&& other) noexcept = default;
    VideoFrame& operator=(VideoFrame&& other) noexcept = default;
};

} // namespace live_assistant
