#pragma once

#include "scene_manager/icapture_source.h"
#include <memory>

namespace live_assistant {

class CaptureFactory {
public:
    // Create a capture source for the given config. For now prefers WGC (if available), else PrintWindow.
    static std::shared_ptr<ICaptureSource> create_capture_source(const CaptureConfig& config);
};

} // namespace live_assistant

