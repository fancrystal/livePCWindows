#pragma once

#include <string>
#include <memory>
#include <vector>

#include "common/error.h"
#include "common/media_clock.h"

namespace live_assistant {

// Output abstract interface
class Output {
public:
    enum class Type {
        FILE_OUTPUT,
        STREAM_OUTPUT,
        DISPLAY_OUTPUT,
        NULL_OUTPUT
    };

    Output(const std::string& id, Type type) : id_(id), type_(type) {}
    virtual ~Output() = default;

    // Get output ID
    std::string get_id() const {
        return id_;
    }

    // Get output type
    Type get_type() const {
        return type_;
    }

    // Initialize output
    virtual ErrorCode initialize() = 0;

    // Start output
    virtual ErrorCode start() = 0;

    // Stop output
    virtual ErrorCode stop() = 0;

    // Shutdown output
    virtual ErrorCode shutdown() = 0;

    // Write encoded audio data
    virtual ErrorCode write_audio_data(const std::vector<uint8_t>& data, MediaTimeUs timestamp) = 0;

    // Write encoded video data
    virtual ErrorCode write_video_data(const std::vector<uint8_t>& data, MediaTimeUs timestamp, bool is_keyframe) = 0;

    // Check if output is running
    virtual bool is_running() const = 0;

    // Get output metadata
    virtual std::string get_metadata() const = 0;

protected:
    std::string id_;
    Type type_;
};

} // namespace live_assistant