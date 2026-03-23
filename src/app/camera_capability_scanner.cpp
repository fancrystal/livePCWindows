#include "app/camera_capability_scanner.h"
#include "common/log.h"

#include <QMetaType>
#include <algorithm>

extern "C" {
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
}

namespace live_assistant {

// 注册元类型以便在信号槽中传递
static const int s_cameraCapabilitiesTypeId = qRegisterMetaType<CameraCapabilities>("CameraCapabilities");

CameraCapabilityScanner::CameraCapabilityScanner(QObject* parent)
    : QObject(parent)
{
}

CameraCapabilityScanner::~CameraCapabilityScanner() = default;

CameraCapabilities CameraCapabilityScanner::query_capabilities(
    const std::string& device_id,
    CaptureMode capture_mode)
{
    LOG_INFO("CameraCapabilityScanner: query_capabilities for " + device_id +
             " mode=" + capture_mode_to_string(capture_mode));

    // 暂时使用默认能力列表，避免阻塞 UI
    // TODO: 可以在后台线程中查询真实能力并缓存
    return get_default_capabilities();
}

void CameraCapabilityScanner::query_capabilities_async(
    const std::string& device_id,
    CaptureMode capture_mode)
{
    // 直接在主线程中返回默认能力，避免线程问题
    CameraCapabilities caps = get_default_capabilities();
    emit capabilities_ready(QString::fromStdString(device_id), caps);
}

CameraCapabilities CameraCapabilityScanner::get_default_capabilities() {
    CameraCapabilities caps;

    // 默认能力列表（常见摄像头支持的模式）
    // MJPG 适合高分辨率，YUY2 适合低分辨率，I420/NV12 部分摄像头支持
    caps.modes.push_back({640, 360, 30, {PixelFormat::YUY2, PixelFormat::MJPG}});
    caps.modes.push_back({640, 480, 30, {PixelFormat::YUY2, PixelFormat::MJPG, PixelFormat::NV12, PixelFormat::I420}});
    caps.modes.push_back({1280, 720, 30, {PixelFormat::YUY2, PixelFormat::MJPG, PixelFormat::NV12}});
    caps.modes.push_back({1280, 720, 60, {PixelFormat::MJPG}});
    caps.modes.push_back({1920, 1080, 30, {PixelFormat::MJPG}});
    caps.modes.push_back({1920, 1080, 60, {PixelFormat::MJPG}});

    return caps;
}

} // namespace live_assistant