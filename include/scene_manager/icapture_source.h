#pragma once

#include <string>
#include <vector>
#include <chrono>
#include "common/media_clock.h"
#include <QImage>
#include <QObject>

namespace live_assistant {

// 采集模式
enum class CaptureMode {
    OPENCV,
    FFMPEG
};

// 像素格式
enum class PixelFormat {
    UNKNOWN,
    YUY2,
    NV12,
    MJPG,
    RGBA,
    I420
};

// 单个摄像头模式
struct CameraMode {
    int width = 0;
    int height = 0;
    int fps = 30;
    std::vector<PixelFormat> supported_formats;

    std::string to_string() const {
        return std::to_string(width) + "x" + std::to_string(height) + "@" + std::to_string(fps) + "fps";
    }

    std::string resolution_string() const {
        return std::to_string(width) + "x" + std::to_string(height);
    }
};

// 摄像头能力集
struct CameraCapabilities {
    std::string device_id;
    std::string device_name;
    std::vector<CameraMode> modes;

    bool empty() const { return modes.empty(); }
    size_t size() const { return modes.size(); }
};

struct CaptureConfig {
    enum class TargetType { SCREEN, WINDOW, CAMERA };
    TargetType type = TargetType::WINDOW;

    // For SCREEN: monitor id
    // For WINDOW: hwnd as string
    // For CAMERA: dshow device_name or OpenCV index as string
    std::string target_id;

    // Display name for UI (e.g., "USB Camera HD" instead of "@device_pnp_...")
    std::string display_name;

    int fps = 30;

    // SCREEN/WINDOW options
    bool capture_cursor = true;
    bool capture_border = true;

    // WGC 采集优化选项
    bool prefer_low_resolution = false;  // 采集时降分辨率（对于外接屏高分辨率场景）
    int reduce_to_width = 1280;           // 降分辨率目标宽度
    int reduce_to_height = 720;           // 降分辨率目标高度

    // CAMERA options
    bool mirror = false;

    // CAMERA 扩展参数
    CaptureMode capture_mode = CaptureMode::FFMPEG;  // 采集模式，默认 FFmpeg
    int width = 640;                                 // 分辨率宽度
    int height = 360;                                // 分辨率高度 (16:9)
    PixelFormat pixel_format = PixelFormat::YUY2;    // 像素格式
    int opencv_index = 0;                            // OpenCV 摄像头索引

    // 辅助方法：获取分辨率字符串
    std::string resolution_string() const {
        return std::to_string(width) + "x" + std::to_string(height);
    }

    // 辅助方法：设置分辨率
    void set_resolution(const std::string& res) {
        size_t pos = res.find('x');
        if (pos != std::string::npos) {
            width = std::stoi(res.substr(0, pos));
            height = std::stoi(res.substr(pos + 1));
        }
    }
};

struct CaptureFrame {
    QImage image; // captured image (BGRA)
    MediaTimestamp timestamp;
    int width = 0;
    int height = 0;
};

class ICaptureSource : public QObject {
    Q_OBJECT
public:
    virtual ~ICaptureSource() = default;

    // Lifecycle
    virtual bool initialize() = 0;
    virtual bool start() = 0;
    virtual bool stop() = 0;
    virtual bool shutdown() = 0;

    virtual const CaptureConfig& get_config() const = 0;
    virtual bool is_running() const = 0;

signals:
    void frameReady(const CaptureFrame& frame);

    // Emitted when the capture source encounters a fatal initialization error.
    // source_id: the capture target id (e.g. "\\.\DISPLAY2")
    // error_message: human-readable description for UI display
    void captureError(const QString& source_id, const QString& error_message);
};

// 辅助函数：像素格式转换
inline std::string pixel_format_to_string(PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::YUY2: return "YUY2";
        case PixelFormat::NV12: return "NV12";
        case PixelFormat::MJPG: return "MJPG";
        case PixelFormat::RGBA: return "RGBA";
        case PixelFormat::I420: return "I420";
        default: return "UNKNOWN";
    }
}

inline PixelFormat string_to_pixel_format(const std::string& str) {
    if (str == "YUY2" || str == "PIXEL_FORMAT_YUY2") return PixelFormat::YUY2;
    if (str == "NV12" || str == "PIXEL_FORMAT_NV12") return PixelFormat::NV12;
    if (str == "MJPG" || str == "PIXEL_FORMAT_MJPG") return PixelFormat::MJPG;
    if (str == "RGBA" || str == "PIXEL_FORMAT_RGBA") return PixelFormat::RGBA;
    if (str == "I420" || str == "PIXEL_FORMAT_I420") return PixelFormat::I420;
    return PixelFormat::UNKNOWN;
}

inline std::string capture_mode_to_string(CaptureMode mode) {
    return mode == CaptureMode::FFMPEG ? "FFMPEG" : "OPENCV";
}inline CaptureMode string_to_capture_mode(const std::string& str) {
    return str == "OPENCV" ? CaptureMode::OPENCV : CaptureMode::FFMPEG;
}} // namespace live_assistant