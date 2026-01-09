#include "scene_manager/capture_factory.h"
#include "scene_manager/printwindow_capture_source.h"
#include "scene_manager/wgc_capture_stub.h"
#include "common/log.h"

namespace live_assistant {

std::unique_ptr<ICaptureSource> CaptureFactory::create_capture_source(const CaptureConfig& config) {
    // Simplified: do not use WGC. Use PrintWindowCaptureSource for both window and screen captures.
    LOG_INFO("CaptureFactory: creating PrintWindowCaptureSource (WGC disabled)");
    return std::make_unique<PrintWindowCaptureSource>(config);
}

} // namespace live_assistant

