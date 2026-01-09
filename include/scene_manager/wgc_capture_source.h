#pragma once

#include "scene_manager/icapture_source.h"
#include "scene_manager/capture_source.h"
#include <memory>
#include <QImage>

struct ID3D11Texture2D;
enum DXGI_FORMAT;

namespace live_assistant {

// Adapter that wraps existing CaptureSource implementation to ICaptureSource.
class WGCaptureSourceAdapter : public ICaptureSource {
public:
    explicit WGCaptureSourceAdapter(const CaptureConfig& cfg);
    ~WGCaptureSourceAdapter() override;

    bool initialize() override;
    bool start() override;
    bool stop() override;
    bool shutdown() override;

    void set_frame_callback(CaptureFrameCallback cb) override;
    const CaptureConfig& get_config() const override { return cfg_; }

private:
    // Helper method to convert D3D11 texture to QImage
    QImage d3d_texture_to_qimage(ID3D11Texture2D* texture, uint32_t width, uint32_t height, DXGI_FORMAT format);

    CaptureConfig cfg_;
    std::shared_ptr<CaptureSource> inner_;
    // Optional PrintWindow fallback (ICaptureSource implementation)
    std::unique_ptr<ICaptureSource> pw_fallback_;
    bool using_fallback_ = false;
    CaptureFrameCallback frame_cb_;
    // Health check for WGC -> if many consecutive null conversions, fall back to PrintWindow
    int consecutive_null_frames_ = 0;
    int fallback_threshold_ = 1; // fallback immediately on first unusable frame to improve UX
};

} // namespace live_assistant

