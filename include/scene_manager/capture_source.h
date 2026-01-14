#pragma once

#include <memory>
#include <string>
#include <functional>
#include <chrono>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.capture.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

namespace live_assistant {

class CaptureSource {
public:
    enum class TargetType {
        SCREEN,
        WINDOW
    };

    struct CaptureConfig {
        TargetType type;
        std::string target_id;  // Monitor device name or window handle as string
        int fps = 30;
        bool capture_cursor = true;
        bool capture_border = true;
    };

    struct FrameData {
        ID3D11Texture2D* texture = nullptr;
        std::chrono::microseconds timestamp;
        uint32_t width = 0;
        uint32_t height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
    };

    using FrameCallback = std::function<void(const FrameData&)>;

    CaptureSource(const CaptureConfig& config);
    ~CaptureSource();

    // Lifecycle
    bool initialize();
    bool start();
    bool stop();
    bool shutdown();

    // Configuration
    void set_frame_callback(FrameCallback callback);
    const CaptureConfig& get_config() const { return config_; }

    // Status
    bool is_initialized() const { return initialized_; }
    bool is_running() const { return running_; }

private:
    // WinRT capture objects
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem capture_item_{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool_{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession capture_session_{ nullptr };
    winrt::event_token frame_arrived_token_{};

    // Direct3D objects
    ID3D11Device* d3d_device_ = nullptr;
    ID3D11DeviceContext* d3d_context_ = nullptr;
    IDXGIFactory2* dxgi_factory_ = nullptr;

    // Configuration and state
    CaptureConfig config_;
    bool initialized_ = false;
    bool running_ = false;
    FrameCallback frame_callback_;
    std::mutex frame_callback_mutex_;
    std::atomic<bool> shutting_down_{false};
    // Initialization thread (for MTA/WinRT work)
    std::thread init_thread_;
    std::atomic<bool> init_thread_stop_{false};
    std::mutex init_mutex_;
    std::condition_variable init_cv_;

    // Frame processing
    void on_frame_arrived(
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
        winrt::Windows::Foundation::IInspectable const& args
    );

    // Helper methods
    bool create_d3d_device();
    bool create_capture_item();
    bool create_frame_pool();
    bool create_capture_session();
    void process_frame(winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame const& frame);
    void cleanup_resources();
    void save_texture_to_png(ID3D11Texture2D* texture, const std::string& filename);
    void probe_surface_adapter(winrt::Windows::Foundation::IInspectable const& surface);
    bool surface_adapter_probed_ = false;
};

} // namespace live_assistant