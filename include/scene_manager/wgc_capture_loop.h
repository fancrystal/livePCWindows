#pragma once

#include "scene_manager/icapture_source.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include <QImage>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace live_assistant {

// Win32CaptureSample-style capture implementation.
// - Uses Direct3D11CaptureFramePool::Create (not FreeThreaded)
// - Creates a DXGI swapchain and copies the arriving frame into the swapchain backbuffer
// - Presents swapchain (like sample)
// - Additionally reads back into QImage and dispatches callback
class WGCCaptureLoop {
public:
    using ImageCallback = std::function<void(const QImage&)>;

    explicit WGCCaptureLoop(const CaptureConfig& cfg);
    ~WGCCaptureLoop();

    bool start();
    void stop();

    void set_frame_callback(ImageCallback cb);

private:
    void thread_proc();

    bool init_d3d();
    bool init_capture_item();
    bool init_capture_objects();

    void on_frame_arrived(
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
        winrt::Windows::Foundation::IInspectable const& args);

    bool try_resize_swapchain(winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame const& frame);
    void resize_swapchain();

    QImage copy_texture_to_qimage(ID3D11Texture2D* src);

    CaptureConfig cfg_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::thread worker_;

    std::mutex cb_mutex_;
    ImageCallback cb_;

    // WinRT capture objects (owned by capture thread)
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool_{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{ nullptr };
    winrt::Windows::Graphics::SizeInt32 last_size_{};

    // D3D
    winrt::com_ptr<ID3D11Device> d3d_device_;
    winrt::com_ptr<ID3D11DeviceContext> d3d_context_;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrt_device_{ nullptr };
    winrt::Windows::Graphics::DirectX::DirectXPixelFormat pixel_format_ = winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized;

    winrt::com_ptr<IDXGISwapChain1> swapchain_;
};

} // namespace live_assistant
