#pragma once

#include <functional>
#include <string>
#include <chrono>
#include "common/media_clock.h"
#include <QImage>

namespace live_assistant {

// use MediaTimestamp defined in common/media_clock.h

struct CaptureConfig {
    enum class TargetType { SCREEN, WINDOW };
    TargetType type = TargetType::WINDOW;
    std::string target_id; // hwnd as string or monitor id
    int fps = 30;
    bool capture_cursor = true;
    bool capture_border = true;
};

struct CaptureFrame {
    QImage image; // captured image (BGRA)
    MediaTimestamp timestamp;
    int width = 0;
    int height = 0;
};

using CaptureFrameCallback = std::function<void(const CaptureFrame&)>;

class ICaptureSource {
public:
    virtual ~ICaptureSource() = default;

    // Lifecycle
    virtual bool initialize() = 0;
    virtual bool start() = 0;
    virtual bool stop() = 0;
    virtual bool shutdown() = 0;

    // Configuration
    virtual void set_frame_callback(CaptureFrameCallback cb) = 0;
    virtual const CaptureConfig& get_config() const = 0;
};

} // namespace live_assistant

