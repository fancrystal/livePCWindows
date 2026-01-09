#include "scene_manager/capture_source.h"
#include "common/log.h"
#include "common/error.h"
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

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowsapp.lib")

namespace live_assistant {

// Helper function to convert HRESULT to hex string
inline std::string hr_to_hex_string(HRESULT hr) {
    char buf[16];
    sprintf_s(buf, sizeof(buf), "%08X", hr);
    return std::string(buf);
}

// Use the Windows SDK helper function (if available) or implement fallback
// Note: GetDXGIInterfaceFromObject might not be available in all SDK versions,
// so we implement it manually using the interop interface
inline HRESULT GetDXGIInterfaceFromObject(winrt::Windows::Foundation::IInspectable const& object,
                                         REFIID iid, void** ppv) {
    *ppv = nullptr;
    void* abiPtr = winrt::get_abi(object);
    if (!abiPtr) {
        return E_POINTER;
    }

    IUnknown* unk = reinterpret_cast<IUnknown*>(abiPtr);

    // Use the proper GUID for the interop interface
    static const GUID IID_IDirect3DDxgiInterfaceAccess = {
        0xa9b3d012, 0x3df2, 0x4ee3, 0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x8f, 0xf8, 0x8b };

    void* rawAccess = nullptr;
    HRESULT hrAccess = unk->QueryInterface(IID_IDirect3DDxgiInterfaceAccess, &rawAccess);
    if (FAILED(hrAccess)) {
        return hrAccess;
    }

    // Use function pointer for GetInterface method
    typedef HRESULT(__stdcall* GetInterfaceFunc)(void*, REFIID, void**);
    GetInterfaceFunc getInterface = reinterpret_cast<GetInterfaceFunc>(
        *reinterpret_cast<void**>(reinterpret_cast<char*>(rawAccess) + 3 * sizeof(void*)));

    HRESULT hrGet = getInterface(rawAccess, iid, ppv);
    static_cast<IUnknown*>(rawAccess)->Release();
    return hrGet;
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
        // Perform WinRT/D3D/WGC initialization in a dedicated MTA thread to avoid
        // conflicting COM apartments (AudioEngine may have initialized STA on main thread).
        std::promise<bool> init_promise;
        std::future<bool> init_future = init_promise.get_future();

        std::thread init_thread([this, p = std::move(init_promise)]() mutable {
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                LOG_DEBUG("init thread: winrt::init_apartment(multi_threaded) called");
            } catch (const winrt::hresult_error& ex) {
                LOG_WARNING(std::string("init thread: winrt::init_apartment hresult_error: hr=0x") +
                            hr_to_hex_string(ex.code().value) + " msg=" + winrt::to_string(ex.message()));
                // If apartment already initialized differently, we cannot proceed here.
                p.set_value(false);
                return;
            } catch (const std::exception& ex) {
                LOG_WARNING(std::string("init thread: winrt::init_apartment failed: ") + ex.what());
                p.set_value(false);
                return;
            }

            // Now safe to create D3D device / capture item / frame pool / session on this thread
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

            // Start capture in this thread (safe because we created session here)
            try {
                capture_session_.StartCapture();
                running_ = true;
                LOG_INFO("init thread: capture session started");
            } catch (const winrt::hresult_error& ex) {
                LOG_WARNING(std::string("init thread: StartCapture hresult_error: hr=0x") +
                            hr_to_hex_string(ex.code().value));
                // still consider initialized but not running
            } catch (...) {
                LOG_WARNING("init thread: StartCapture unknown error");
            }

            p.set_value(true);
        });

        // Wait for initialization to complete with timeout
        auto status = init_future.wait_for(std::chrono::seconds(5));
        bool ok = false;
        if (status == std::future_status::ready) {
            ok = init_future.get();
        } else {
            LOG_ERROR("Timed out waiting for WGC initialization thread");
        }

        // join init thread
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
        // Start the capture session
        capture_session_.StartCapture();

        running_ = true;
        LOG_INFO("CaptureSource started successfully");
        return true;

    } catch (const winrt::hresult_error& ex) {
        LOG_ERROR("WinRT error during start: " + winrt::to_string(ex.message()));
        return false;
    } catch (const std::exception& ex) {
        LOG_ERROR("Exception during start: " + std::string(ex.what()));
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
        }

        running_ = false;
        LOG_INFO("CaptureSource stopped successfully");
        return true;

    } catch (const winrt::hresult_error& ex) {
        LOG_ERROR("WinRT error during stop: " + winrt::to_string(ex.message()));
        return false;
    } catch (const std::exception& ex) {
        LOG_ERROR("Exception during stop: " + std::string(ex.what()));
        return false;
    }
}

bool CaptureSource::shutdown() {
    LOG_INFO("Shutting down CaptureSource");

    // Stop if running
    stop();

    // Clean up resources
    cleanup_resources();

    initialized_ = false;
    LOG_INFO("CaptureSource shutdown complete");
    return true;
}

void CaptureSource::set_frame_callback(FrameCallback callback) {
    frame_callback_ = callback;
}

bool CaptureSource::create_d3d_device() {
    LOG_INFO("Creating D3D11 device");

    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDevice(
        nullptr,                    // adapter
        D3D_DRIVER_TYPE_HARDWARE,   // driver type
        nullptr,                    // software
        flags,                      // flags
        feature_levels,             // feature levels
        ARRAYSIZE(feature_levels),  // num feature levels
        D3D11_SDK_VERSION,          // SDK version
        &d3d_device_,               // device
        nullptr,                    // feature level
        &d3d_context_               // context
    );

    if (FAILED(hr)) {
        LOG_ERROR("Failed to create D3D11 device: " + std::to_string(hr));
        return false;
    }

    // Get DXGI factory
    IDXGIDevice* dxgi_device = nullptr;
    hr = d3d_device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device));
    if (FAILED(hr)) {
        LOG_ERROR("Failed to get DXGI device: " + std::to_string(hr));
        return false;
    }

    IDXGIAdapter* dxgi_adapter = nullptr;
    hr = dxgi_device->GetAdapter(&dxgi_adapter);
    if (FAILED(hr)) {
        dxgi_device->Release();
        LOG_ERROR("Failed to get DXGI adapter: " + std::to_string(hr));
        return false;
    }

    hr = dxgi_adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&dxgi_factory_));
    dxgi_adapter->Release();
    dxgi_device->Release();

    if (FAILED(hr)) {
        LOG_ERROR("Failed to get DXGI factory: " + std::to_string(hr));
        return false;
    }

    LOG_INFO("D3D11 device created successfully");
    // Enumerate adapters for diagnostics
    if (dxgi_factory_) {
        UINT i = 0;
        IDXGIAdapter1* adapter = nullptr;
        while (dxgi_factory_->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND) {
            DXGI_ADAPTER_DESC1 desc1;
            if (SUCCEEDED(adapter->GetDesc1(&desc1))) {
                uint64_t luidHigh = desc1.AdapterLuid.HighPart;
                uint64_t luidLow = desc1.AdapterLuid.LowPart;
                // Convert wide description to narrow string (best-effort)
                std::wstring wdesc(desc1.Description);
                std::string desc_narrow(wdesc.begin(), wdesc.end());
                LOG_DEBUG("Enumerated adapter index=" + std::to_string(i) +
                          " Description=" + desc_narrow +
                          " VendorId=" + std::to_string(desc1.VendorId) +
                          " DeviceId=" + std::to_string(desc1.DeviceId) +
                          " LUID=" + std::to_string(luidHigh) + "/" + std::to_string(luidLow));
            } else {
                LOG_DEBUG("Enumerated adapter index=" + std::to_string(i) + " (GetDesc1 failed)");
            }
            adapter->Release();
            adapter = nullptr;
            ++i;
        }
    }
    return true;
}

bool CaptureSource::create_capture_item() {
    LOG_INFO("Creating capture item for target: " + config_.target_id);

    try {
        if (config_.type == TargetType::SCREEN) {
            // Create capture item for monitor
            HMONITOR hMonitor = nullptr;

            // Parse monitor device name or find primary monitor
            if (!config_.target_id.empty()) {
                // Try to find monitor by device name
                MONITORINFOEXA info = {};
                info.cbSize = sizeof(MONITORINFOEXA);

                std::pair<std::string, HMONITOR*> params = {config_.target_id, &hMonitor};
                EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMon, HDC, LPRECT, LPARAM lParam) -> BOOL {
                    auto* params = reinterpret_cast<std::pair<std::string, HMONITOR*>*>(lParam);
                    MONITORINFOEXA info = {};
                    info.cbSize = sizeof(MONITORINFOEXA);

                    if (GetMonitorInfoA(hMon, &info)) {
                        if (params->first == info.szDevice) {
                            *params->second = hMon;
                            return FALSE; // Stop enumeration
                        }
                    }
                    return TRUE; // Continue enumeration
                }, reinterpret_cast<LPARAM>(&params));

                if (!hMonitor) {
                    LOG_WARNING("Monitor not found, using primary monitor");
                    hMonitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
                }
            } else {
                hMonitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
            }

            // Create capture item from monitor
            auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                IGraphicsCaptureItemInterop>();
            HRESULT hr = interop->CreateForMonitor(hMonitor, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                winrt::put_abi(capture_item_));

            if (FAILED(hr)) {
                LOG_ERROR("Failed to create capture item for monitor: " + std::to_string(hr));
                return false;
            }

        } else { // WINDOW
            // Create capture item for window
            HWND hwnd = nullptr;

            try {
                // Parse window handle from string
                uintptr_t hwnd_val = std::stoull(config_.target_id);
                hwnd = reinterpret_cast<HWND>(hwnd_val);
            } catch (const std::exception&) {
                LOG_ERROR("Invalid window handle: " + config_.target_id);
                return false;
            }

            // Verify window exists and is visible
            if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
                LOG_ERROR("Window is not valid or not visible: " + config_.target_id);
                return false;
            }

            // Create capture item from window
            auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                IGraphicsCaptureItemInterop>();
            HRESULT hr = interop->CreateForWindow(hwnd, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                winrt::put_abi(capture_item_));

            if (FAILED(hr)) {
                LOG_ERROR("Failed to create capture item for window: " + std::to_string(hr));
                return false;
            }
        }

        LOG_INFO("Capture item created successfully");
        return true;

    } catch (const winrt::hresult_error& ex) {
        LOG_ERROR("WinRT error creating capture item: " + winrt::to_string(ex.message()));
        return false;
    } catch (const std::exception& ex) {
        LOG_ERROR("Exception creating capture item: " + std::string(ex.what()));
        return false;
    }
}

bool CaptureSource::create_frame_pool() {
    LOG_INFO("Creating frame pool");

    try {
        // Get the size of the item
        auto size = capture_item_.Size();

        // Log local D3D device adapter info for debugging
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(d3d_device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice)) && dxgiDevice) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter) {
                DXGI_ADAPTER_DESC desc{};
                adapter->GetDesc(&desc);
                LOG_DEBUG("Local D3D adapter LUID: " + std::to_string(desc.AdapterLuid.HighPart) + "/" + std::to_string(desc.AdapterLuid.LowPart));
                adapter->Release();
            }
            // keep dxgiDevice alive for CreateDirect3D11DeviceFromDXGIDevice below
        }

        // Create WinRT D3D device wrapper
        winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrtDevice{ nullptr };
        if (!dxgiDevice) {
            // try to acquire again (shouldn't usually fail)
            HRESULT hrQ = d3d_device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
            if (FAILED(hrQ) || !dxgiDevice) {
                LOG_ERROR("Failed to obtain IDXGIDevice for CreateDirect3D11DeviceFromDXGIDevice: " + std::to_string(hrQ));
                return false;
            }
        }

        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
            dxgiDevice, reinterpret_cast<IInspectable**>(winrt::put_abi(winrtDevice))));

        // Create frame pool using CreateFreeThreaded (like demo)
        frame_pool_ = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrtDevice,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            6, // Buffer count (like demo)
            size);

        // Set up frame arrived event
        frame_pool_.FrameArrived({ this, &CaptureSource::on_frame_arrived });

        // release temporary dxgiDevice we kept
        if (dxgiDevice) {
            dxgiDevice->Release();
            dxgiDevice = nullptr;
        }

        LOG_INFO("Frame pool created successfully (CreateFreeThreaded)");
        return true;

    } catch (const winrt::hresult_error& ex) {
        LOG_ERROR("WinRT error creating frame pool: " + winrt::to_string(ex.message()));
        return false;
    } catch (const std::exception& ex) {
        LOG_ERROR("Exception creating frame pool: " + std::string(ex.what()));
        return false;
    }
}

bool CaptureSource::create_capture_session() {
    LOG_INFO("Creating capture session");

    try {
        // Create capture session
        capture_session_ = frame_pool_.CreateCaptureSession(capture_item_);

        // Configure session
        capture_session_.IsCursorCaptureEnabled(config_.capture_cursor);
        capture_session_.IsBorderRequired(config_.type == TargetType::WINDOW && config_.capture_border);

        LOG_INFO("Capture session created successfully");
        return true;

    } catch (const winrt::hresult_error& ex) {
        LOG_ERROR("WinRT error creating capture session: " + winrt::to_string(ex.message()));
        return false;
    } catch (const std::exception& ex) {
        LOG_ERROR("Exception creating capture session: " + std::string(ex.what()));
        return false;
    }
}

void CaptureSource::on_frame_arrived(
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
    winrt::Windows::Foundation::IInspectable const& args
) {
    try {
        // Try to get the latest frame
        auto frame = sender.TryGetNextFrame();
        if (frame) {
            process_frame(frame);
        }
    } catch (const winrt::hresult_error& ex) {
        LOG_ERROR("WinRT error in frame arrived: " + winrt::to_string(ex.message()));
    } catch (const std::exception& ex) {
        LOG_ERROR("Exception in frame arrived: " + std::string(ex.what()));
    }
}

void CaptureSource::process_frame(winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame const& frame) {
    try {
        // Get the Direct3D surface
        auto surface = frame.Surface();

        // Get the texture from the surface: try IDirect3DDxgiInterfaceAccess -> as<ID3D11Texture2D> -> QI(ID3D11Texture2D)
        winrt::com_ptr<ID3D11Texture2D> d3d_texture;

        // 1) Try GetDXGIInterfaceFromObject helper (like demo)
        void* rawTex = nullptr;
        HRESULT hrTex = GetDXGIInterfaceFromObject(surface, __uuidof(ID3D11Texture2D), &rawTex);
        if (SUCCEEDED(hrTex) && rawTex) {
            d3d_texture.attach(static_cast<ID3D11Texture2D*>(rawTex));
            LOG_DEBUG("CaptureSource::process_frame: obtained ID3D11Texture2D via GetDXGIInterfaceFromObject");
        } else {
            LOG_INFO("CaptureSource::process_frame: GetDXGIInterfaceFromObject failed, hr=0x" + hr_to_hex_string(hrTex) + " (" + std::to_string(hrTex) + ")");

            // 2) Fallback: try direct WinRT projection to ID3D11Texture2D
            try {
                d3d_texture = surface.as<ID3D11Texture2D>();
                LOG_DEBUG("CaptureSource::process_frame: surface.as<ID3D11Texture2D>() succeeded");
            } catch (const winrt::hresult_error& ex) {
                LOG_INFO("CaptureSource::process_frame: surface.as<ID3D11Texture2D>() failed: " + winrt::to_string(ex.message()) + " (hr=0x" + hr_to_hex_string(ex.code().value) + ")");
            } catch (...) {
                LOG_INFO("CaptureSource::process_frame: surface.as<ID3D11Texture2D>() unknown failure");
            }
        }

        // 2) Fallback: try direct WinRT projection to ID3D11Texture2D
        if (!d3d_texture) {
            try {
                d3d_texture = surface.as<ID3D11Texture2D>();
                LOG_DEBUG("CaptureSource::process_frame: surface.as<ID3D11Texture2D>() succeeded");
            } catch (const winrt::hresult_error& ex) {
                LOG_INFO("CaptureSource::process_frame: surface.as<ID3D11Texture2D>() failed: " + winrt::to_string(ex.message()));
            } catch (...) {
                LOG_INFO("CaptureSource::process_frame: surface.as<ID3D11Texture2D>() unknown failure");
            }
        }

        // 3) Last resort: QueryInterface on IUnknown for ID3D11Texture2D
        if (!d3d_texture) {
            void* abiPtr2 = winrt::get_abi(surface);
            if (abiPtr2) {
                IUnknown* unk2 = reinterpret_cast<IUnknown*>(abiPtr2);
                void* rawTexture = nullptr;
                HRESULT hrQI = unk2->QueryInterface(__uuidof(ID3D11Texture2D), &rawTexture);
                if (SUCCEEDED(hrQI) && rawTexture) {
                    d3d_texture.attach(static_cast<ID3D11Texture2D*>(rawTexture));
                    LOG_DEBUG("CaptureSource::process_frame: QI(ID3D11Texture2D) succeeded on IUnknown");
                } else {
                    LOG_INFO("CaptureSource::process_frame: QI(ID3D11Texture2D) failed, hr=" + std::to_string(hrQI));
                }
            }
        }

        // If still no texture, probe which adapter the surface belongs to (once)
        if (!d3d_texture && !surface_adapter_probed_) {
            probe_surface_adapter(surface);
        }

        // Get texture description
        if (!d3d_texture) {
            LOG_ERROR("CaptureSource::process_frame: retrieved d3d_texture is null, aborting frame processing");
            return;
        }
        D3D11_TEXTURE2D_DESC desc = {};
        d3d_texture.get()->GetDesc(&desc);

        // Save first frame to PNG for debugging (only once)
        static bool first_frame_saved = false;
        if (!first_frame_saved) {
            save_texture_to_png(d3d_texture.get(), "first_wgc_frame.png");
            first_frame_saved = true;
            LOG_INFO("First WGC frame saved to first_wgc_frame.png for debugging");
        }

        // For now, use the texture directly without synchronization
        ID3D11Texture2D* sync_texture = d3d_texture.get();

        // Create frame data
        FrameData frame_data;
        frame_data.texture = sync_texture;
        frame_data.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch());
        frame_data.width = desc.Width;
        frame_data.height = desc.Height;
        frame_data.format = desc.Format;

        // Call frame callback if set
        if (frame_callback_) {
            frame_callback_(frame_data);
        }

        // Release synchronized texture if different from original
        if (sync_texture != d3d_texture.get()) {
            sync_texture->Release();
        }

    } catch (const winrt::hresult_error& ex) {
        LOG_ERROR("WinRT error processing frame: " + winrt::to_string(ex.message()));
    } catch (const std::exception& ex) {
        LOG_ERROR("Exception processing frame: " + std::string(ex.what()));
    }
}

void CaptureSource::cleanup_resources() {
    LOG_INFO("Cleaning up CaptureSource resources");

    try {
        if (capture_session_) {
            capture_session_.Close();
            capture_session_ = nullptr;
        }

        if (frame_pool_) {
            frame_pool_.Close();
            frame_pool_ = nullptr;
        }

        capture_item_ = nullptr;

        if (dxgi_factory_) {
            dxgi_factory_->Release();
            dxgi_factory_ = nullptr;
        }

        if (d3d_context_) {
            d3d_context_->Release();
            d3d_context_ = nullptr;
        }

        if (d3d_device_) {
            d3d_device_->Release();
            d3d_device_ = nullptr;
        }

    } catch (const std::exception& ex) {
        LOG_ERROR("Exception during cleanup: " + std::string(ex.what()));
    }

    LOG_INFO("CaptureSource resources cleaned up");
}

void CaptureSource::save_texture_to_png(ID3D11Texture2D* texture, const std::string& filename) {
    if (!texture || !d3d_device_) {
        LOG_ERROR("Invalid texture or device for PNG saving");
        return;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);

    // Create staging texture for CPU read access
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ID3D11Texture2D* stagingTexture = nullptr;
    HRESULT hr = d3d_device_->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create staging texture for PNG: " + hr_to_hex_string(hr));
        return;
    }

    // Copy texture to staging
    ID3D11DeviceContext* context = nullptr;
    d3d_device_->GetImmediateContext(&context);
    if (!context) {
        LOG_ERROR("Failed to get immediate context for PNG save");
        stagingTexture->Release();
        return;
    }

    context->CopyResource(stagingTexture, texture);

    // Map staging texture
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(hr)) {
        LOG_INFO("Mapped staging texture successfully: " + std::to_string(desc.Width) + "x" + std::to_string(desc.Height) +
                " format=" + std::to_string(desc.Format) + " rowPitch=" + std::to_string(mapped.RowPitch));

        // Convert BGRA to QImage ARGB and save to desktop
        int w = static_cast<int>(desc.Width);
        int h = static_cast<int>(desc.Height);
        QImage img(w, h, QImage::Format_ARGB32);
        for (int y = 0; y < h; ++y) {
            const uint8_t* srcLine = reinterpret_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
            uint32_t* dstLine = reinterpret_cast<uint32_t*>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                uint8_t b = srcLine[x * 4 + 0];
                uint8_t g = srcLine[x * 4 + 1];
                uint8_t r = srcLine[x * 4 + 2];
                uint8_t a = srcLine[x * 4 + 3];
                dstLine[x] = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
                             (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
            }
        }

        QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
        QString path = desktop + "/" + QString::fromStdString(filename);
        if (img.save(path)) {
            LOG_INFO("Saved WGC frame PNG to: " + path.toStdString());
        } else {
            LOG_ERROR("Failed to save WGC frame PNG to: " + path.toStdString());
        }

        context->Unmap(stagingTexture, 0);
    } else {
        LOG_ERROR("Failed to map staging texture: " + hr_to_hex_string(hr));
    }

    context->Release();
    stagingTexture->Release();
}


void CaptureSource::probe_surface_adapter(winrt::Windows::Foundation::IInspectable const& surface) {
    surface_adapter_probed_ = true;
    LOG_INFO("Probing surface adapter for diagnostics");

    void* px = nullptr;
    HRESULT hr = GetDXGIInterfaceFromObject(surface, __uuidof(IDXGIDevice), &px);
    if (SUCCEEDED(hr) && px) {
        IDXGIDevice* idxDev = static_cast<IDXGIDevice*>(px);
        IDXGIAdapter* adapter = nullptr;
        HRESULT hrA = idxDev->GetAdapter(&adapter);
        if (SUCCEEDED(hrA) && adapter) {
            DXGI_ADAPTER_DESC desc;
            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                std::wstring wdesc(desc.Description);
                std::string desc_narrow(wdesc.begin(), wdesc.end());
                uint64_t luidHigh = desc.AdapterLuid.HighPart;
                uint64_t luidLow = desc.AdapterLuid.LowPart;
                LOG_INFO("Surface IDXGIDevice -> Adapter: Description=" + desc_narrow +
                         " VendorId=" + std::to_string(desc.VendorId) +
                         " DeviceId=" + std::to_string(desc.DeviceId) +
                         " LUID=" + std::to_string(luidHigh) + "/" + std::to_string(luidLow));
            } else {
                LOG_INFO("Surface adapter: GetDesc failed");
            }
            adapter->Release();
        } else {
            LOG_INFO("probe_surface_adapter: GetAdapter failed, hr=" + hr_to_hex_string(hrA));
        }
        idxDev->Release();
    } else {
        LOG_INFO("probe_surface_adapter: GetDXGIInterfaceFromObject(IDXGIDevice) failed, hr=0x" + hr_to_hex_string(hr));
    }
}


} // namespace live_assistant