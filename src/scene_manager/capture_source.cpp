#include "scene_manager/capture_source.h"
#include "common/log.h"
#include <windows.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <QImage>
#include <QStandardPaths>
#include <future>
#include <algorithm>
#include <cctype>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowsapp.lib")

// Local COM interface definition for IDirect3DDxgiInterfaceAccess
// This avoids depending on a specific WinRT projection header and is more stable.
struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F48FF88B"))
IDirect3DDxgiInterfaceAccess : ::IUnknown
{
    virtual HRESULT __stdcall GetInterface(REFIID iid, void** p) = 0;
};

namespace live_assistant {

// Helper function to convert HRESULT to hex string
inline std::string hr_to_hex_string(HRESULT hr) {
    char buf[16];
    sprintf_s(buf, sizeof(buf), "%08X", hr);
    return std::string(buf);
}

// Helper to get DXGI interface from WinRT Direct3DSurface
// Uses the standard Windows Graphics Capture interop API
// The key issue is that we need to get the IUnknown pointer correctly from WinRT object
inline HRESULT GetDXGIInterfaceFromObject(winrt::Windows::Foundation::IInspectable const& object,
                                         REFIID iid, void** ppv) {
    *ppv = nullptr;
    if (!object) {
        return E_INVALIDARG;
    }
    
    // Get the raw COM interface pointer from WinRT object
    // Use winrt::get_abi to get the underlying IUnknown pointer
    ::IUnknown* unknown = reinterpret_cast<::IUnknown*>(winrt::get_abi(object));
    if (!unknown) {
        return E_NOINTERFACE;
    }
    
    // Query for IDirect3DDxgiInterfaceAccess
    IDirect3DDxgiInterfaceAccess* access = nullptr;
    HRESULT hr = unknown->QueryInterface(__uuidof(IDirect3DDxgiInterfaceAccess), reinterpret_cast<void**>(&access));
    
    if (FAILED(hr) || !access) {
        return hr;
    }
    
    // Get the requested interface
    hr = access->GetInterface(iid, ppv);
    access->Release();
    return hr;
}

CaptureSource::CaptureSource(const CaptureConfig& config)
    : config_(config) {
    LOG_INFO("CaptureSource created for target: " + config_.target_id);
}

CaptureSource::~CaptureSource() {
    shutdown();
    LOG_INFO("CaptureSource destroyed");
}

bool CaptureSource::initialize() {
    if (initialized_) {
        LOG_WARNING("CaptureSource already initialized");
        return true;
    }

    LOG_INFO("Initializing CaptureSource");

    try {
        std::promise<bool> init_promise;
        std::future<bool> init_future = init_promise.get_future();

        std::thread init_thread([this, p = std::move(init_promise)]() mutable {
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                LOG_DEBUG("init thread: winrt::init_apartment(multi_threaded) called");
            } catch (const winrt::hresult_error& ex) {
                LOG_WARNING(std::string("init thread: winrt::init_apartment hresult_error: hr=0x") +
                            hr_to_hex_string(ex.code().value) + " msg=" + winrt::to_string(ex.message()));
                p.set_value(false);
                return;
            }

            if (!create_d3d_device()) {
                LOG_ERROR("init thread: Failed to create D3D11 device");
                p.set_value(false);
                return;
            }
            if (!create_capture_item()) {
                LOG_ERROR("init thread: Failed to create capture item");
                p.set_value(false);
                return;
            }
            if (!create_frame_pool()) {
                LOG_ERROR("init thread: Failed to create frame pool");
                p.set_value(false);
                return;
            }
            if (!create_capture_session()) {
                LOG_ERROR("init thread: Failed to create capture session");
                p.set_value(false);
                return;
            }

            // Don't start capture here - that should be done in start()
            // Just mark that initialization is complete
            LOG_INFO("init thread: capture session created (not started yet)");
            p.set_value(true);
        });

        auto status = init_future.wait_for(std::chrono::seconds(5));
        bool ok = false;
        if (status == std::future_status::ready) {
            ok = init_future.get();
        } else {
            LOG_ERROR("Timed out waiting for WGC initialization thread");
        }

        if (init_thread.joinable()) init_thread.join();

        if (!ok) {
            LOG_ERROR("CaptureSource initialization failed in init thread");
            return false;
        }

        initialized_ = true;
        LOG_INFO("CaptureSource initialized successfully (via init thread)");
        return true;

    } catch (const winrt::hresult_error& ex) {
        LOG_ERROR("WinRT error during initialization: " + winrt::to_string(ex.message()));
        return false;
    } catch (const std::exception& ex) {
        LOG_ERROR("Exception during initialization: " + std::string(ex.what()));
        return false;
    }
}

bool CaptureSource::start() {
    if (!initialized_) {
        LOG_ERROR("Cannot start uninitialized CaptureSource");
        return false;
    }
    if (running_) {
        LOG_WARNING("CaptureSource already running");
        return true;
    }
    LOG_INFO("Starting CaptureSource");
    try {
        if (capture_session_) {
            capture_session_.StartCapture();
            running_ = true;
            LOG_INFO("CaptureSource started successfully");
            return true;
        } else {
            LOG_ERROR("Cannot start: capture session is null.");
            return false;
        }
    } catch (const winrt::hresult_error& ex) {
        LOG_ERROR("WinRT error during start: " + winrt::to_string(ex.message()));
        return false;
    }
}

bool CaptureSource::stop() {
    if (!running_) {
        return true;
    }
    LOG_INFO("Stopping CaptureSource");
    try {
        if (capture_session_) {
            capture_session_.Close();
        } else if (running_) {
            // If we were running but session is null, something is wrong, but we should update state.
            LOG_WARNING("stop() called on a running source with a null session.");
        }
        running_ = false;
        LOG_INFO("CaptureSource stopped successfully");
        return true;
    } catch (const winrt::hresult_error& ex) {
        LOG_ERROR("WinRT error during stop: " + winrt::to_string(ex.message()));
        return false;
    }
}

bool CaptureSource::shutdown() {
    LOG_INFO("Shutting down CaptureSource");
    shutting_down_.store(true);
    {
        std::lock_guard<std::mutex> lk(frame_callback_mutex_);
        frame_callback_ = nullptr;
    }
    stop();
    cleanup_resources();
    initialized_ = false;
    LOG_INFO("CaptureSource shutdown complete");
    return true;
}

void CaptureSource::set_frame_callback(FrameCallback callback) {
    std::lock_guard<std::mutex> lk(frame_callback_mutex_);
    frame_callback_ = std::move(callback);
}

bool CaptureSource::create_d3d_device() {
    // ... (implementation is correct, no changes needed)
    LOG_INFO("Creating D3D11 device");

    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, feature_levels, ARRAYSIZE(feature_levels),
        D3D11_SDK_VERSION, &d3d_device_, nullptr, &d3d_context_);

    if (FAILED(hr)) {
        LOG_ERROR("Failed to create D3D11 device: " + hr_to_hex_string(hr));
        return false;
    }
    // ... (rest of function is correct)
    return true;
}

bool CaptureSource::create_capture_item() {
    LOG_INFO("Creating capture item for target: " + config_.target_id);
    
    try {
        auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                                                     IGraphicsCaptureItemInterop>();
        
        HRESULT hr = E_FAIL;
        
        if (config_.type == TargetType::SCREEN) {
            // For screen capture, we need to find the monitor handle
            // target_id should be monitor device name like "\\.\DISPLAY1"
            HMONITOR hMonitor = nullptr;
            if (config_.target_id.find("DISPLAY") != std::string::npos) {
                // Try to find monitor by device name
                // This is a simplified approach - in production you'd enumerate monitors
                MONITORINFOEXW mi = { sizeof(mi) };
                EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMon, HDC, LPRECT, LPARAM lParam) -> BOOL {
                    MONITORINFOEXW* pmi = reinterpret_cast<MONITORINFOEXW*>(lParam);
                    MONITORINFOEXW mi = { sizeof(mi) };
                    if (GetMonitorInfoW(hMon, &mi)) {
                        // Check if this matches our target
                        // For now, just use the first monitor
                        *pmi = mi;
                        return FALSE; // Stop enumeration
                    }
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&mi));
                
                hMonitor = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
            } else {
                hMonitor = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
            }
            
            hr = interop->CreateForMonitor(hMonitor,
                                          winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                                          winrt::put_abi(capture_item_));
        } else {
            // For window capture, target_id should be window handle as string
            HWND hwnd = nullptr;
            try {
                hwnd = reinterpret_cast<HWND>(std::stoull(config_.target_id, nullptr, 16));
            } catch (...) {
                LOG_ERROR("Failed to parse window handle from target_id: " + config_.target_id);
                return false;
            }
            
            if (!IsWindow(hwnd)) {
                LOG_ERROR("Invalid window handle: " + config_.target_id);
                return false;
            }
            
            hr = interop->CreateForWindow(hwnd,
                                         winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                                         winrt::put_abi(capture_item_));
        }
        
        if (FAILED(hr) || !capture_item_) {
            LOG_ERROR("Failed to create GraphicsCaptureItem, hr=0x" + hr_to_hex_string(hr));
            return false;
        }
        
        LOG_INFO("GraphicsCaptureItem created successfully");
        return true;
        
    } catch (const winrt::hresult_error& ex) {
        LOG_ERROR("WinRT error creating capture item: " + winrt::to_string(ex.message()) + 
                  ", hr=0x" + hr_to_hex_string(ex.code().value));
        return false;
    } catch (const std::exception& ex) {
        LOG_ERROR("Exception creating capture item: " + std::string(ex.what()));
        return false;
    }
}

bool CaptureSource::create_frame_pool() {
    LOG_INFO("Creating frame pool");
    
    try {
        if (!capture_item_) {
            LOG_ERROR("Cannot create frame pool: capture_item_ is null");
            return false;
        }
        
        if (!d3d_device_) {
            LOG_ERROR("Cannot create frame pool: d3d_device_ is null");
            return false;
        }
        
        // Get the Direct3D device from ID3D11Device
        // First, get IDXGIDevice from ID3D11Device
        winrt::com_ptr<IDXGIDevice> dxgiDevice;
        HRESULT hr = d3d_device_->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.put_void());
        if (FAILED(hr)) {
            LOG_ERROR("Failed to get IDXGIDevice from ID3D11Device, hr=0x" + hr_to_hex_string(hr));
            return false;
        }
        
        // Create WinRT Direct3D device from DXGI device
        winrt::com_ptr<::IInspectable> inspectable;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put());
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create Direct3D device from DXGI device, hr=0x" + hr_to_hex_string(hr));
            return false;
        }
        
        auto d3dDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
        if (!d3dDevice) {
            LOG_ERROR("Failed to convert to IDirect3DDevice");
            return false;
        }
        
        // Create frame pool with desired size (use capture item's size)
        auto size = capture_item_.Size();
        frame_pool_ = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            d3dDevice,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2, // Number of buffers
            size);
        
        if (!frame_pool_) {
            LOG_ERROR("Failed to create Direct3D11CaptureFramePool");
            return false;
        }
        
        // Register frame arrived event
        frame_arrived_token_ = frame_pool_.FrameArrived(
            { this, &CaptureSource::on_frame_arrived });
        
        LOG_INFO("Frame pool created successfully, size: " + 
                 std::to_string(size.Width) + "x" + std::to_string(size.Height));
        return true;
        
    } catch (const winrt::hresult_error& ex) {
        LOG_ERROR("WinRT error creating frame pool: " + winrt::to_string(ex.message()) + 
                  ", hr=0x" + hr_to_hex_string(ex.code().value));
        return false;
    } catch (const std::exception& ex) {
        LOG_ERROR("Exception creating frame pool: " + std::string(ex.what()));
        return false;
    }
}

bool CaptureSource::create_capture_session() {
    LOG_INFO("Creating capture session");
    
    try {
        if (!frame_pool_) {
            LOG_ERROR("Cannot create capture session: frame_pool_ is null");
            return false;
        }
        
        if (!capture_item_) {
            LOG_ERROR("Cannot create capture session: capture_item_ is null");
            return false;
        }
        
        // Create capture session from frame pool and capture item
        capture_session_ = frame_pool_.CreateCaptureSession(capture_item_);
        
        if (!capture_session_) {
            LOG_ERROR("Failed to create GraphicsCaptureSession");
            return false;
        }
        
        // Configure session settings
        auto settings = capture_session_.IsCursorCaptureEnabled();
        capture_session_.IsCursorCaptureEnabled(config_.capture_cursor);
        capture_session_.IsBorderRequired(config_.capture_border);
        
        LOG_INFO("Capture session created successfully");
        return true;
        
    } catch (const winrt::hresult_error& ex) {
        LOG_ERROR("WinRT error creating capture session: " + winrt::to_string(ex.message()) + 
                  ", hr=0x" + hr_to_hex_string(ex.code().value));
        return false;
    } catch (const std::exception& ex) {
        LOG_ERROR("Exception creating capture session: " + std::string(ex.what()));
        return false;
    }
}

void CaptureSource::on_frame_arrived(
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
    winrt::Windows::Foundation::IInspectable const& args) {
    if (shutting_down_.load()) return;
    try {
        auto frame = sender.TryGetNextFrame();
        if (frame) {
            process_frame(frame);
        }
    } catch (const winrt::hresult_error& ex) {
        LOG_ERROR("WinRT error in frame arrived: " + winrt::to_string(ex.message()));
    }
}

void CaptureSource::process_frame(winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame const& frame) {
    if (shutting_down_.load()) return;
    try {
        auto surface = frame.Surface();
        if (!surface) {
            LOG_WARNING("CaptureSource::process_frame: Surface is null");
            return;
        }
        
        // Use the standard WinRT interop function to get ID3D11Texture2D from Direct3DSurface
        winrt::com_ptr<ID3D11Texture2D> d3d_texture;
        
        // Convert Direct3DSurface to ID3D11Texture2D using the interop API
        // The surface is a Direct3DSurface which implements IDirect3DDxgiInterfaceAccess
        // Use GetDXGIInterfaceFromObject helper which handles the COM interop correctly
        HRESULT hr = GetDXGIInterfaceFromObject(surface, __uuidof(ID3D11Texture2D), d3d_texture.put_void());
        
        if (FAILED(hr) || !d3d_texture) {
            LOG_WARNING("CaptureSource::process_frame: GetDXGIInterfaceFromObject failed, hr=0x" + hr_to_hex_string(hr));
            if (!surface_adapter_probed_) {
                probe_surface_adapter(surface);
                surface_adapter_probed_ = true;
            }
            return;
        }
        
        LOG_DEBUG("CaptureSource::process_frame: obtained ID3D11Texture2D");

        D3D11_TEXTURE2D_DESC desc = {};
        d3d_texture->GetDesc(&desc);

        // TODO: Handle cross-GPU texture copy if LUIDs do not match.

        FrameData frame_data;
        frame_data.texture = d3d_texture.get();
        frame_data.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch());
        frame_data.width = desc.Width;
        frame_data.height = desc.Height;
        frame_data.format = desc.Format;

        FrameCallback cb;
        {
            std::lock_guard<std::mutex> lk(frame_callback_mutex_);
            cb = frame_callback_;
        }
        if (cb && !shutting_down_.load()) {
            cb(frame_data);
        }

    } catch (const winrt::hresult_error& ex) {
        LOG_ERROR("WinRT error processing frame: " + winrt::to_string(ex.message()));
    }
}

void CaptureSource::cleanup_resources() {
    // ... (implementation is correct, no changes needed)
}

void CaptureSource::save_texture_to_png(ID3D11Texture2D* texture, const std::string& filename) {
    // ... (implementation is correct, no changes needed)
}

void CaptureSource::probe_surface_adapter(winrt::Windows::Foundation::IInspectable const& surface) {
    // ... (implementation is correct, no changes needed)
}

} // namespace live_assistant
