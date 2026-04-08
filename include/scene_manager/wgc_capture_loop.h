#pragma once

#include "scene_manager/icapture_source.h"
#include "scene_manager/shared_d3d_device.h"
#include "scene_manager/gpu_texture_ref.h"

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
    using ImageCallback   = std::function<void(const QImage&)>;
    // Phase 1: GPU 纹理直传回调（GPU→GPU CopyResource，无 CPU 回读）
    using TextureCallback = std::function<void(const GpuTextureRef&)>;

    explicit WGCCaptureLoop(const CaptureConfig& cfg);
    ~WGCCaptureLoop();

    bool start();
    void stop();

    // 原有 CPU 图像回调（兼容保留）
    void set_frame_callback(ImageCallback cb);

    // Phase 1: 设置 GPU 纹理回调
    // 设置后，每帧将通过 GPU→GPU CopyResource 生成 DEFAULT 纹理并回调，
    // 同时跳过 CPU 回读（copy_texture_to_qimage 不再执行）
    void set_texture_callback(TextureCallback cb);

    // 更新捕获设置（运行时生效，无需重启捕获）
    void update_settings(bool capture_cursor, bool capture_border);

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

    // Phase 1: GPU→GPU CopyResource 到 DEFAULT 纹理，返回 GpuTextureRef
    GpuTextureRef create_gpu_frame_copy(ID3D11Texture2D* src);

    CaptureConfig cfg_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::thread worker_;

    // 采集节流：限制到目标帧率（用于外接屏 60fps 场景）
    static constexpr int TARGET_INTERVAL_MS = 33;  // 30fps ≈ 33ms/帧
    std::chrono::steady_clock::time_point last_processed_time_{};
    std::mutex throttle_mutex_;

    // 诊断日志：每秒输出采集帧率
    std::atomic<int> frame_count_{0};
    std::chrono::steady_clock::time_point last_fps_log_{};
    std::mutex fps_mutex_;

    std::mutex cb_mutex_;
    ImageCallback cb_;

    // Phase 1: GPU 纹理回调及缓存
    std::mutex tex_cb_mutex_;
    TextureCallback tex_cb_;
    winrt::com_ptr<ID3D11Texture2D> cached_gpu_frame_texture_;
    uint32_t cached_gpu_frame_width_  = 0;
    uint32_t cached_gpu_frame_height_ = 0;
    uint64_t frame_id_counter_        = 0;

    // WinRT capture objects (owned by capture thread)
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool_{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{ nullptr };
    winrt::Windows::Graphics::SizeInt32 last_size_{};

    // D3D (uses shared device from SharedD3D11Device)
    winrt::Windows::Graphics::DirectX::DirectXPixelFormat pixel_format_ = winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized;

    winrt::com_ptr<IDXGISwapChain1> swapchain_;

    // Cached staging texture for CPU readback (avoid per-frame allocation)
    winrt::com_ptr<ID3D11Texture2D> cached_staging_texture_;
    uint32_t cached_staging_width_ = 0;
    uint32_t cached_staging_height_ = 0;
};

} // namespace live_assistant
