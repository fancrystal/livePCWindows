#pragma once

#include <string>
#include <memory>

#include "common/error.h"
#include "common/media_clock.h"

namespace live_assistant {

// Forward declarations
struct AudioFrame;
struct VideoFrame;

// Media source abstract interface
class Source {
public:
    enum class Type {
        AUDIO_CAPTURE,
        VIDEO_CAPTURE,
        SCREEN_CAPTURE,
        FILE_SOURCE,
        NETWORK_SOURCE
    };

    Source(const std::string& id, Type type) : id_(id), type_(type) {}
    virtual ~Source() = default;

    // Get source ID
    std::string get_id() const {
        return id_;
    }

    // Get source type
    Type get_type() const {
        return type_;
    }

    // Initialize source
    virtual bool initialize() = 0;

    // Start source
    virtual bool start() = 0;

    // Stop source
    virtual bool stop() = 0;

    // Shutdown source
    virtual bool shutdown() = 0;

    // Get audio frame (non-blocking)
    virtual std::shared_ptr<AudioFrame> get_audio_frame() = 0;

    // Get video frame (non-blocking)
    virtual std::shared_ptr<VideoFrame> get_video_frame() = 0;

    // Check if source is running
    virtual bool is_running() const = 0;

    // Get source metadata
    virtual std::string get_metadata() const = 0;

protected:
    std::string id_;
    Type type_;
};

} // namespace live_assistant
