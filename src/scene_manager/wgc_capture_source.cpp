#include "scene_manager/wgc_capture_source.h"
#include "scene_manager/capture_source.h"
#include "common/log.h"
#include "common/media_clock.h"
#include "scene_manager/printwindow_capture_source.h"
#include <QImage>
#include <d3d11.h>

namespace live_assistant {

WGCaptureSourceAdapter::WGCaptureSourceAdapter(const CaptureConfig& cfg)
    : cfg_(cfg) {
}

WGCaptureSourceAdapter::~WGCaptureSourceAdapter() {
    shutdown();
}

bool WGCaptureSourceAdapter::initialize() {
    // Map our CaptureConfig to inner CaptureSource::CaptureConfig
    CaptureSource::CaptureConfig inner_cfg;
    inner_cfg.type = (cfg_.type == CaptureConfig::TargetType::SCREEN) ? CaptureSource::TargetType::SCREEN : CaptureSource::TargetType::WINDOW;
    inner_cfg.target_id = cfg_.target_id;
    inner_cfg.fps = cfg_.fps;
    inner_cfg.capture_cursor = cfg_.capture_cursor;
    inner_cfg.capture_border = cfg_.capture_border;

    inner_ = std::make_shared<CaptureSource>(inner_cfg);
    return inner_->initialize();
}

bool WGCaptureSourceAdapter::start() {
    if (!inner_) return false;

    // Set frame callback to forward to our CaptureFrameCallback by converting types
    inner_->set_frame_callback([this](const CaptureSource::FrameData& fd) {
        LOG_DEBUG("WGCaptureSourceAdapter: received FrameData: " + std::to_string(fd.width) + "x" + std::to_string(fd.height));
        if (!frame_cb_) {
            LOG_DEBUG("WGCaptureSourceAdapter: no frame callback set, dropping frame");
            return;
        }

        CaptureFrame out;
        out.width = fd.width;
        out.height = fd.height;
        out.timestamp = MediaClock().now();

        if (fd.texture) {
            out.image = d3d_texture_to_qimage(fd.texture, fd.width, fd.height, fd.format);
            if (out.image.isNull()) {
                LOG_WARNING("WGCaptureSourceAdapter: converted QImage is null");
                // health check -> maybe WGC not providing usable textures; try fallback after threshold
                consecutive_null_frames_++;
                if (consecutive_null_frames_ >= fallback_threshold_) {
                    LOG_WARNING("WGCaptureSourceAdapter: consecutive null frames threshold reached, falling back to PrintWindowCaptureSource");
                    try {
                        pw_fallback_ = std::make_unique<PrintWindowCaptureSource>(cfg_);
                        // Bind callback for PrintWindow (it uses CaptureFrame type via ICaptureSource)
                        pw_fallback_->set_frame_callback([this](const CaptureFrame& cf) {
                            if (frame_cb_) frame_cb_(cf);
                        });
                        if (pw_fallback_->initialize() && pw_fallback_->start()) {
                            LOG_INFO("WGCaptureSourceAdapter: started PrintWindow fallback");
                            // stop original inner capture
                            if (inner_) {
                                inner_->shutdown();
                                inner_.reset();
                            }
                            using_fallback_ = true;
                        } else {
                            LOG_ERROR("WGCaptureSourceAdapter: failed to initialize/start PrintWindow fallback");
                            pw_fallback_.reset();
                        }
                    } catch (const std::exception& ex) {
                        LOG_ERROR("WGCaptureSourceAdapter: exception creating PrintWindow fallback: " + std::string(ex.what()));
                        pw_fallback_.reset();
                    }
                    consecutive_null_frames_ = 0;
                }
            } else {
                LOG_INFO("WGCaptureSourceAdapter: converted QImage size: " + std::to_string(out.image.width()) + "x" + std::to_string(out.image.height()));
                consecutive_null_frames_ = 0;
            }
        } else {
            LOG_WARNING("WGCaptureSourceAdapter: FrameData.texture is null");
            consecutive_null_frames_++;
            if (consecutive_null_frames_ >= fallback_threshold_) {
                LOG_WARNING("WGCaptureSourceAdapter: consecutive null textures threshold reached");
                consecutive_null_frames_ = 0;
            }
        }

        frame_cb_(out);
    });

    return inner_->start();
}

bool WGCaptureSourceAdapter::stop() {
    if (using_fallback_) {
        if (pw_fallback_) return pw_fallback_->stop();
        return true;
    }
    if (!inner_) return true;
    return inner_->stop();
}

bool WGCaptureSourceAdapter::shutdown() {
    bool ok = true;
    if (pw_fallback_) {
        ok = pw_fallback_->shutdown();
        pw_fallback_.reset();
    }
    if (inner_) {
        inner_->shutdown();
        inner_.reset();
    }
    using_fallback_ = false;
    return ok;
}

void WGCaptureSourceAdapter::set_frame_callback(CaptureFrameCallback cb) {
    frame_cb_ = cb;
    // Propagate to active underlying source(s)
    if (pw_fallback_) {
        pw_fallback_->set_frame_callback(frame_cb_);
    }
    if (inner_) {
        // inner_ expects CaptureSource::FrameCallback; we keep adapter mapping in start()
    }
}

QImage WGCaptureSourceAdapter::d3d_texture_to_qimage(ID3D11Texture2D* texture, uint32_t width, uint32_t height, DXGI_FORMAT format) {
    if (!texture || width == 0 || height == 0) {
        return QImage();
    }

    // Get the inner CaptureSource's D3D device context for mapping
    if (!inner_ || !inner_->is_initialized()) {
        return QImage();
    }

    // We need access to the D3D device context from CaptureSource
    // For now, create a temporary device context to map the texture
    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;

    texture->GetDevice(&d3dDevice);
    if (!d3dDevice) {
        LOG_ERROR("WGCaptureSourceAdapter: Could not get D3D device from texture");
        return QImage();
    }

    d3dDevice->GetImmediateContext(&d3dContext);
    if (!d3dContext) {
        LOG_ERROR("WGCaptureSourceAdapter: Could not get D3D device context");
        d3dDevice->Release();
        return QImage();
    }

    // Create a staging texture for CPU access
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = width;
    stagingDesc.Height = height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = format; // Use the same format as the source texture
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.SampleDesc.Quality = 0;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ID3D11Texture2D* stagingTexture = nullptr;
    HRESULT hr = d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
    if (FAILED(hr)) {
        LOG_ERROR("WGCaptureSourceAdapter: Failed to create staging texture");
        d3dContext->Release();
        d3dDevice->Release();
        return QImage();
    }

    // Copy the texture to staging
    LOG_DEBUG("WGCaptureSourceAdapter: created staging texture, copying resource");
    d3dContext->CopyResource(stagingTexture, texture);

    // Map the staging texture
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    hr = d3dContext->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mappedResource);
    if (FAILED(hr)) {
        LOG_ERROR("WGCaptureSourceAdapter: Failed to map staging texture");
        stagingTexture->Release();
        d3dContext->Release();
        d3dDevice->Release();
        return QImage();
    }
    LOG_DEBUG("WGCaptureSourceAdapter: mapped staging texture, RowPitch=" + std::to_string(mappedResource.RowPitch));

    // Create QImage and copy data
    QImage image;
    if (format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        // BGRA format - Qt expects RGBA, so we need to convert
        image = QImage(width, height, QImage::Format_RGBA8888);

        const uint8_t* srcData = static_cast<const uint8_t*>(mappedResource.pData);
        uint8_t* dstData = image.bits();
        int srcStride = mappedResource.RowPitch;
        int dstStride = image.bytesPerLine();

        for (uint32_t y = 0; y < height; ++y) {
            const uint32_t* srcRow = reinterpret_cast<const uint32_t*>(srcData + y * srcStride);
            uint32_t* dstRow = reinterpret_cast<uint32_t*>(dstData + y * dstStride);

            for (uint32_t x = 0; x < width; ++x) {
                uint32_t bgra = srcRow[x];
                // Convert BGRA to RGBA
                uint8_t b = (bgra >> 0) & 0xFF;
                uint8_t g = (bgra >> 8) & 0xFF;
                uint8_t r = (bgra >> 16) & 0xFF;
                uint8_t a = (bgra >> 24) & 0xFF;
                dstRow[x] = (r) | (g << 8) | (b << 16) | (a << 24);
            }
        }
    } else {
        // For other formats, create a placeholder for now
        LOG_WARNING("WGCaptureSourceAdapter: Unsupported texture format, using placeholder");
        image = QImage(width, height, QImage::Format_RGBA8888);
        image.fill(QColor(200, 100, 150)); // Placeholder purple color
    }

    // Unmap and cleanup
    d3dContext->Unmap(stagingTexture, 0);
    stagingTexture->Release();
    d3dContext->Release();
    d3dDevice->Release();
    LOG_DEBUG("WGCaptureSourceAdapter: finished conversion to QImage");
    return image;
}

} // namespace live_assistant

