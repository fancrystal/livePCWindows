#pragma once

#include <d3d11.h>
#include <d3d11_4.h>   // ID3D11Multithread (in 4.h), ID3D11VideoDevice/Context
#include <dxgi1_2.h>
#include <mutex>
#include <cstdint>
#include <string>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

// Forward declare QImage to avoid Qt dependency cascade in headers that
// include shared_d3d_device.h but don't otherwise need Qt.
class QImage;

namespace live_assistant {

// Shared D3D11 device manager for WGC capture sources.
// Multiple capture sources share the same device to reduce GPU overhead.
//
// Phase 0 enhancements:
//   - enable_multithread_protection(): thread-safe context access
//   - video_device() / video_context(): for D3D11VideoProcessor (GPU color convert)
//   - create_texture_2d() / create_srv() / create_rtv(): convenience helpers
class SharedD3D11Device {
public:
    static SharedD3D11Device& instance();

    // Get the shared D3D11 device (creates on first call)
    ID3D11Device* device();
    ID3D11DeviceContext* context();

    // Get the WinRT wrapped device for WGC API
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrt_device();

    // Check if device is valid
    bool is_valid() const { return d3d_device_ != nullptr; }

    // ---------------------------------------------------------------
    // Phase 1.1: GPU vendor detection
    // ---------------------------------------------------------------
    // Identifies which GPU vendor backs the shared D3D11 device. Used by
    // the multi-GPU rendering pipeline to pick the right encoder candidate
    // and to enable / disable vendor-specific GPU paths (e.g. Intel RC/CCS
    // workaround, NVENC zero-copy, AMF zero-copy).
    enum class GpuVendor {
        Unknown = 0,
        Intel   = 1,    // VendorId 0x8086 — pairs with QSV
        NVIDIA  = 2,    // VendorId 0x10DE — pairs with NVENC
        AMD     = 3,    // VendorId 0x1002 — pairs with AMF
        Other   = 4,    // anything else (Microsoft Basic Render, virtual GPUs, etc.)
    };

    // Returns the vendor of the currently selected adapter.
    // Triggers init() lazily if the device has not been created yet.
    GpuVendor vendor();

    // Returns the human-readable adapter description (UTF-8).
    // Empty string if the device has not been created yet.
    std::string adapter_name();

    // ---------------------------------------------------------------
    // Phase 0: multithread protection
    // ---------------------------------------------------------------
    // Enable ID3D11Multithread protection so that the D3D11 immediate
    // context can be called safely from multiple threads.
    // Must be called before any concurrent GPU work begins.
    // Returns false only if the device does not support it (shouldn't happen).
    bool enable_multithread_protection();

    // ---------------------------------------------------------------
    // Phase 0: Video Processing interfaces (used by GpuColorConverter)
    // ---------------------------------------------------------------
    // Returns the ID3D11VideoDevice interface (lazily queried from device_).
    // Returns nullptr if the GPU doesn't support D3D11 video processing.
    ID3D11VideoDevice* video_device();

    // Returns the ID3D11VideoContext interface (lazily queried from context_).
    ID3D11VideoContext* video_context();

    // ---------------------------------------------------------------
    // Phase 0: texture / view creation helpers
    // ---------------------------------------------------------------
    // Create a 2D texture. Caller owns the returned com_ptr.
    winrt::com_ptr<ID3D11Texture2D> create_texture_2d(
        uint32_t width, uint32_t height,
        DXGI_FORMAT format,
        D3D11_USAGE usage     = D3D11_USAGE_DEFAULT,
        UINT bind_flags       = D3D11_BIND_SHADER_RESOURCE,
        UINT cpu_access_flags = 0,
        UINT misc_flags       = 0);

    // Create a Shader Resource View for a texture.
    winrt::com_ptr<ID3D11ShaderResourceView> create_srv(
        ID3D11Texture2D* texture,
        DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM);

    // Create a Render Target View for a texture.
    winrt::com_ptr<ID3D11RenderTargetView> create_rtv(
        ID3D11Texture2D* texture,
        DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM);

    // ---------------------------------------------------------------
    // Phase 1b: CPU image → GPU texture upload
    // ---------------------------------------------------------------
    // Upload a QImage (any Qt ARGB/RGBA format, converted to BGRA internally)
    // to a reusable DYNAMIC D3D11 texture.
    // If `reuse` is non-null and its size matches, it is updated in-place
    // (avoids re-allocation). Pass nullptr to always create a fresh texture.
    // Returns a valid com_ptr on success, empty on failure.
    //
    // Thread-safety: call enable_multithread_protection() before concurrent use.
    winrt::com_ptr<ID3D11Texture2D> upload_image_to_texture(
        const QImage& image,
        ID3D11Texture2D* reuse = nullptr);

private:
    SharedD3D11Device() = default;
    ~SharedD3D11Device();

    SharedD3D11Device(const SharedD3D11Device&) = delete;
    SharedD3D11Device& operator=(const SharedD3D11Device&) = delete;

    bool init();

    std::mutex mutex_;
    winrt::com_ptr<ID3D11Device>         d3d_device_;
    winrt::com_ptr<ID3D11DeviceContext>  d3d_context_;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrt_device_{ nullptr };

    // Phase 0: lazily initialized
    winrt::com_ptr<ID3D11VideoDevice>    video_device_;
    winrt::com_ptr<ID3D11VideoContext>   video_context_;
    bool multithread_enabled_ = false;

    // Phase 1.1: vendor info captured during init()
    GpuVendor   gpu_vendor_   = GpuVendor::Unknown;
    std::string adapter_name_;
};

} // namespace live_assistant