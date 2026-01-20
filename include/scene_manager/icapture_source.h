#pragma once

#include <string>
#include <chrono>
#include "common/media_clock.h"
#include <QImage>
#include <QObject>

namespace live_assistant {

struct CaptureConfig {
    enum class TargetType { SCREEN, WINDOW, CAMERA };
    TargetType type = TargetType::WINDOW;

    // For SCREEN: monitor id
    // For WINDOW: hwnd as string
    // For CAMERA: dshow device_name or OpenCV index as string
    std::string target_id;

    int fps = 30;

    // SCREEN/WINDOW options
    bool capture_cursor = true;
    bool capture_border = true;
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
};

}

 // namespace live_assistant

