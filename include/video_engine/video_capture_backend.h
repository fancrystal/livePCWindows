#pragma once

#include "common/error.h"
#include "common/media_clock.h"
#include <memory>
#include <string>
#include <vector>

namespace live_assistant {

struct VideoFrame;

/**
 * 视频捕获后端接口
 * 定义了视频捕获后端的基本接口
 */
class VideoCaptureBackend {
public:
    virtual ~VideoCaptureBackend() = default;

    /**
     * 获取可用设备列表
     * @return 设备ID列表
     */
    virtual std::vector<std::string> get_available_devices() = 0;

    /**
     * 选择捕获设备
     * @param device_id 设备ID
     * @return 操作结果
     */
    virtual ErrorCode select_device(const std::string& device_id) = 0;

    /**
     * 捕获一帧视频
     * @param frame 输出帧
     * @return 操作结果
     */
    virtual ErrorCode capture_frame(std::shared_ptr<VideoFrame>& frame) = 0;

    /**
     * 检查后端是否正在运行
     * @return 是否正在运行
     */
    virtual bool is_running() const = 0;

    /**
     * 获取后端名称
     * @return 后端名称
     */
    virtual const char* get_backend_name() const = 0;
};

/**
 * 视频捕获后端工厂类
 * 用于创建不同类型的视频捕获后端实例
 */
class VideoCaptureBackendFactory {
public:
    /**
     * 创建FFmpeg后端实例
     * @return FFmpeg后端实例
     */
    static std::unique_ptr<VideoCaptureBackend> create_ffmpeg_backend();

    /**
     * 创建OpenCV后端实例
     * @return OpenCV后端实例
     */
    static std::unique_ptr<VideoCaptureBackend> create_opencv_backend();

    /**
     * 根据名称创建后端实例
     * @param backend_name 后端名称 ("FFmpeg", "OpenCV")
     * @return 后端实例，如果名称无效则返回nullptr
     */
    static std::unique_ptr<VideoCaptureBackend> create_backend(const std::string& backend_name);
};

} // namespace live_assistant