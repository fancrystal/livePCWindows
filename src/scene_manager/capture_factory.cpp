#include "scene_manager/capture_factory.h"
#include "scene_manager/printwindow_capture_source.h"
#include "scene_manager/wgc_capture_source.h"
#include "common/log.h"

namespace live_assistant {

std::shared_ptr<ICaptureSource> CaptureFactory::create_capture_source(const CaptureConfig& config) {
    // Try WGC first, fallback to PrintWindow if WGC fails
    LOG_INFO("CaptureFactory: attempting to create WGCaptureSourceAdapter for target: " + config.target_id);
    
    try {
        auto wgc_adapter = std::make_shared<WGCaptureSourceAdapter>(config);
        
        // Try to initialize WGC
        if (wgc_adapter->initialize()) {
            LOG_INFO("CaptureFactory: WGCaptureSourceAdapter initialized successfully");
            return wgc_adapter;
        } else {
            LOG_WARNING("CaptureFactory: WGCaptureSourceAdapter initialization failed, falling back to PrintWindowCaptureSource");
            wgc_adapter.reset();
        }
    } catch (const std::exception& ex) {
        LOG_WARNING("CaptureFactory: Exception creating WGCaptureSourceAdapter: " + std::string(ex.what()) + ", falling back to PrintWindowCaptureSource");
    } catch (...) {
        LOG_WARNING("CaptureFactory: Unknown exception creating WGCaptureSourceAdapter, falling back to PrintWindowCaptureSource");
    }
    
    // Fallback to PrintWindow
    LOG_INFO("CaptureFactory: creating PrintWindowCaptureSource as fallback");
    return std::make_shared<PrintWindowCaptureSource>(config);
}

} // namespace live_assistant

