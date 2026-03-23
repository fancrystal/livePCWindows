#include "scene_manager/capture_factory.h"
#include "scene_manager/printwindow_capture_source.h"
#include "scene_manager/wgc_capture_source.h"
#include "scene_manager/opencv_camera_capture_source.h"
#include "scene_manager/ffmpeg_camera_capture_source.h"
#include "common/log.h"

namespace live_assistant {

std::shared_ptr<ICaptureSource> CaptureFactory::create_capture_source(const CaptureConfig& config) {
    if (config.type == CaptureConfig::TargetType::CAMERA) {
        // 根据 capture_mode 选择采集源
        if (config.capture_mode == CaptureMode::OPENCV) {
            // OpenCV 模式
            LOG_INFO("CaptureFactory: creating OpenCVCameraCaptureSource for camera device: " + config.target_id);

            try {
                auto opencv_src = std::make_shared<OpenCVCameraCaptureSource>(config);
                if (opencv_src->initialize()) {
                    LOG_INFO("CaptureFactory: OpenCVCameraCaptureSource initialized successfully");
                    return opencv_src;
                }

                LOG_WARNING("CaptureFactory: OpenCVCameraCaptureSource failed");
            } catch (const std::exception& ex) {
                LOG_ERROR("CaptureFactory: Exception during OpenCV camera source creation: " + std::string(ex.what()));
            } catch (...) {
                LOG_ERROR("CaptureFactory: Unknown exception during OpenCV camera source creation");
            }

            return nullptr;
        } else {
            // FFmpeg 模式（默认）
            LOG_INFO("CaptureFactory: creating FFmpegCameraCaptureSource for camera device: " + config.target_id +
                     " resolution=" + std::to_string(config.width) + "x" + std::to_string(config.height) +
                     " format=" + pixel_format_to_string(config.pixel_format));

            try {
                auto ffmpeg_src = std::make_shared<FFmpegCameraCaptureSource>(config);
                if (ffmpeg_src->initialize()) {
                    LOG_INFO("CaptureFactory: FFmpegCameraCaptureSource initialized successfully");
                    return ffmpeg_src;
                }

                LOG_WARNING("CaptureFactory: FFmpegCameraCaptureSource failed, trying OpenCV as fallback");

                // Fallback to OpenCV if FFmpeg fails
                auto opencv_src = std::make_shared<OpenCVCameraCaptureSource>(config);
                if (opencv_src->initialize()) {
                    LOG_INFO("CaptureFactory: OpenCVCameraCaptureSource initialized successfully as fallback");
                    return opencv_src;
                }

                LOG_ERROR("CaptureFactory: Both FFmpeg and OpenCV failed to initialize camera");
            } catch (const std::exception& ex) {
                LOG_ERROR("CaptureFactory: Exception during camera source creation: " + std::string(ex.what()));
            } catch (...) {
                LOG_ERROR("CaptureFactory: Unknown exception during camera source creation");
            }

            return nullptr;
        }
    }

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

