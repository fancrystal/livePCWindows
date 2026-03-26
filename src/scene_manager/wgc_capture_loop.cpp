#include "scene_manager/wgc_capture_loop.h"
#include "common/log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowsapp.lib")

namespace live_assistant {

namespace {

// Win32CaptureSample uses robmikh::common helpers. We inline the minimal subset.

// Get a DXGI interface from a WinRT object that implements IDirect3DDxgiInterfaceAccess.
template <typename T>
winrt::com_ptr<T> GetDXGIInterfaceFromObject(winrt::Windows::Foundation::IInspectable const& object)
{
    winrt::com_ptr<T> result;

    winrt::com_ptr<::IUnknown> unk = object.as<::IUnknown>();
    winrt::com_ptr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
    winrt::check_hresult(unk->QueryInterface(__uuidof(::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess), access.put_void()));
    winrt::check_hresult(access->GetInterface(__uuidof(T), result.put_void()));

    return result;
}

winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice CreateDirect3DDevice(ID3D11Device* device)
{
    winrt::com_ptr<IDXGIDevice> dxgi;
    winrt::check_hresult(device->QueryInterface(dxgi.put()));

    winrt::com_ptr<::IInspectable> insp;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put()));
    return insp.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
}

winrt::com_ptr<IDXGISwapChain1> CreateSwapChain(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format, uint32_t bufferCount)
{
    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    winrt::check_hresult(device->QueryInterface(dxgiDevice.put()));

    winrt::com_ptr<IDXGIAdapter> adapter;
    winrt::check_hresult(dxgiDevice->GetAdapter(adapter.put()));

    winrt::com_ptr<IDXGIFactory2> factory;
    winrt::check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = format;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = bufferCount;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = 0;

    winrt::com_ptr<IDXGISwapChain1> sc;
    winrt::check_hresult(factory->CreateSwapChainForComposition(device, &desc, nullptr, sc.put()));
    return sc;
}

// Map cfg.target_id to the monitor WGC should capture. Previously SCREEN always used
// MONITOR_DEFAULTTOPRIMARY, so every screen source grabbed the primary display.
static HMONITOR ResolveMonitorFromTargetId(const std::string& target_id)
{
    auto primary = []() {
        return MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
    };

    if (target_id.empty()) {
        return primary();
    }

    // Numeric id: QScreen::handle() / legacy uintptr_t encoding
    const bool all_digits =
        !target_id.empty() &&
        std::all_of(target_id.begin(), target_id.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    if (all_digits) {
        uintptr_t v = std::strtoull(target_id.c_str(), nullptr, 10);
        if (v != 0) {
            HMONITOR h = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(v));
            MONITORINFO mi = {sizeof(mi)};
            if (GetMonitorInfo(h, &mi)) {
                return h;
            }
        }
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, target_id.c_str(), -1, nullptr, 0);
    if (wlen > 1) {
        std::vector<wchar_t> wbuf(static_cast<size_t>(wlen));
        MultiByteToWideChar(CP_UTF8, 0, target_id.c_str(), -1, wbuf.data(), wlen);
        std::wstring want(wbuf.data());

        struct MatchCtx {
            const std::wstring* want;
            HMONITOR found;
        } ctx{&want, nullptr};
        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
                auto* c = reinterpret_cast<MatchCtx*>(lp);
                MONITORINFOEXW mi{};
                mi.cbSize = sizeof(mi);
                if (!GetMonitorInfoW(hMon, reinterpret_cast<LPMONITORINFO>(&mi))) {
                    return TRUE;
                }
                if (_wcsicmp(mi.szDevice, c->want->c_str()) == 0) {
                    c->found = hMon;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));
        if (ctx.found) {
            return ctx.found;
        }
    }

    LOG_WARNING("WGCCaptureLoop: unknown screen target_id='" + target_id + "', using primary monitor");
    return primary();
}

} // namespace

WGCCaptureLoop::WGCCaptureLoop(const CaptureConfig& cfg) : cfg_(cfg) {}
WGCCaptureLoop::~WGCCaptureLoop() { stop(); }

void WGCCaptureLoop::set_frame_callback(ImageCallback cb)
{
    std::lock_guard<std::mutex> lk(cb_mutex_);
    cb_ = std::move(cb);
}

bool WGCCaptureLoop::start()
{
    if (running_.exchange(true)) return true;
    stopping_ = false;
    worker_ = std::thread(&WGCCaptureLoop::thread_proc, this);
    return true;
}

void WGCCaptureLoop::stop()
{
    if (!running_.exchange(false)) return;
    stopping_ = true;

    // No message loop to wake - just wait for thread to exit
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool WGCCaptureLoop::init_d3d()
{
    // Use shared D3D device instead of creating per-instance
    // Call device() to trigger initialization if not already done
    auto& shared = SharedD3D11Device::instance();
    if (!shared.device()) {
        LOG_ERROR("WGCCaptureLoop: SharedD3D11Device initialization failed");
        return false;
    }
    return true;
}

bool WGCCaptureLoop::init_capture_item()
{
    auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();

    HRESULT hr = E_FAIL;
    if (cfg_.type == CaptureConfig::TargetType::SCREEN) {
        HMONITOR mon = ResolveMonitorFromTargetId(cfg_.target_id);
        hr = interop->CreateForMonitor(mon,
                                       winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                                       winrt::put_abi(item_));
    } else {
        HWND hwnd = reinterpret_cast<HWND>(std::stoull(cfg_.target_id));
        hr = interop->CreateForWindow(hwnd,
                                      winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                                      winrt::put_abi(item_));
    }

    if (FAILED(hr) || !item_) {
        LOG_ERROR("WGCCaptureLoop: failed to create GraphicsCaptureItem hr=0x" + std::to_string(static_cast<uint32_t>(hr)));
        return false;
    }

    return true;
}

bool WGCCaptureLoop::init_capture_objects()
{
    last_size_ = item_.Size();

    // Use shared D3D device
    auto& shared = SharedD3D11Device::instance();
    auto winrt_device = shared.winrt_device();
    if (!winrt_device) {
        LOG_ERROR("WGCCaptureLoop: Failed to get shared WinRT device");
        return false;
    }

    auto dxgiDevice = GetDXGIInterfaceFromObject<ID3D11Device>(winrt_device);

    swapchain_ = CreateSwapChain(dxgiDevice.get(), static_cast<uint32_t>(last_size_.Width), static_cast<uint32_t>(last_size_.Height),
                                 static_cast<DXGI_FORMAT>(pixel_format_), 2);

    frame_pool_ = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(winrt_device, pixel_format_, 2, last_size_);
    session_ = frame_pool_.CreateCaptureSession(item_);
    session_.IsCursorCaptureEnabled(cfg_.capture_cursor);
    session_.IsBorderRequired(cfg_.capture_border);

    frame_pool_.FrameArrived({ this, &WGCCaptureLoop::on_frame_arrived });

    return true;
}

void WGCCaptureLoop::resize_swapchain()
{
    try {
        if (!swapchain_) return;
        winrt::check_hresult(swapchain_->ResizeBuffers(2,
            static_cast<uint32_t>(last_size_.Width),
            static_cast<uint32_t>(last_size_.Height),
            static_cast<DXGI_FORMAT>(pixel_format_),
            0));
    } catch (const winrt::hresult_error& ex) {
        LOG_ERROR("[WGC] winrt::hresult_error in resize_swapchain: " + winrt::to_string(ex.message()) +
                 ", code: " + std::to_string(ex.code()));
    } catch (const std::exception& ex) {
        LOG_ERROR("[WGC] std::exception in resize_swapchain: " + std::string(ex.what()));
    } catch (...) {
        LOG_ERROR("[WGC] Unknown exception in resize_swapchain");
    }
}

bool WGCCaptureLoop::try_resize_swapchain(winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame const& frame)
{
    auto cs = frame.ContentSize();
    if (cs.Width != last_size_.Width || cs.Height != last_size_.Height) {
        last_size_ = cs;
        resize_swapchain();
        return true;
    }
    return false;
}

void WGCCaptureLoop::on_frame_arrived(
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
    winrt::Windows::Foundation::IInspectable const&)
{
    try {
        auto frame_start = std::chrono::high_resolution_clock::now();
        auto frame_start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(frame_start.time_since_epoch()).count();
        LOG_DEBUG("[DIAG] WGCCaptureLoop::on_frame_arrived START t=" + std::to_string(frame_start_ms % 100000));
        if (stopping_) return;

    bool resized = false;
    winrt::com_ptr<ID3D11Texture2D> surfaceTexture;

        {
            auto frame = sender.TryGetNextFrame();
            if (!frame) return;

            resized = try_resize_swapchain(frame);

            // back buffer
            winrt::com_ptr<ID3D11Texture2D> backBuffer;
            winrt::check_hresult(swapchain_->GetBuffer(0, winrt::guid_of<ID3D11Texture2D>(), backBuffer.put_void()));

            surfaceTexture = GetDXGIInterfaceFromObject<ID3D11Texture2D>(frame.Surface());
            SharedD3D11Device::instance().context()->CopyResource(backBuffer.get(), surfaceTexture.get());
            
            // 显式关闭frame，尽早释放FramePool的buffer
            frame.Close();
        }

    DXGI_PRESENT_PARAMETERS params{};
    swapchain_->Present1(1, 0, &params);

    if (resized) {
        frame_pool_.Recreate(SharedD3D11Device::instance().winrt_device(), pixel_format_, 2, last_size_);
        return;
    }

    // Read back into QImage (additional step for Qt pipeline)
    if (surfaceTexture) {
        LOG_DEBUG("[DIAG] WGCCaptureLoop::on_frame_arrived - 开始复制纹理到QImage");
        auto t0 = std::chrono::high_resolution_clock::now();
        QImage img = copy_texture_to_qimage(surfaceTexture.get());
        auto t1 = std::chrono::high_resolution_clock::now();
        auto copy_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        LOG_DEBUG("[DIAG] WGCCaptureLoop::on_frame_arrived - QImage创建完成，copy_us=" + std::to_string(copy_us) +
                 ", isNull: " + std::string(img.isNull() ? "true" : "false") +
                 ", 尺寸: " + std::to_string(img.width()) + "x" + std::to_string(img.height()));

        if (!img.isNull()) {
            ImageCallback cb;
            {
                std::lock_guard<std::mutex> lk(cb_mutex_);
                cb = cb_;
            }
            LOG_DEBUG("[DIAG] WGCCaptureLoop::on_frame_arrived - 回调函数存在: " + std::string(cb ? "true" : "false"));
            if (cb) {
                LOG_DEBUG("[DIAG] WGCCaptureLoop::on_frame_arrived - 调用回调函数");
                auto t2 = std::chrono::high_resolution_clock::now();
                cb(img);
                auto t3 = std::chrono::high_resolution_clock::now();
                auto cb_us = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();
                LOG_DEBUG("[DIAG] WGCCaptureLoop::on_frame_arrived - 回调完成 cb_us=" + std::to_string(cb_us));
            } else {
                LOG_DEBUG("[DIAG] WGCCaptureLoop::on_frame_arrived - 回调函数为空");
            }
        } else {
            LOG_DEBUG("[DIAG] WGCCaptureLoop::on_frame_arrived - QImage为空，跳过回调");
        }
        auto frame_end = std::chrono::high_resolution_clock::now();
        auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(frame_end - frame_start).count();
        LOG_DEBUG("[DIAG] WGCCaptureLoop::on_frame_arrived END total_us=" + std::to_string(total_us));
    } else {
        LOG_DEBUG("[DIAG] WGCCaptureLoop::on_frame_arrived - surfaceTexture为空");
    }
    } catch (const winrt::hresult_error& ex) {
        LOG_ERROR("[WGC] winrt::hresult_error in on_frame_arrived: " + winrt::to_string(ex.message()) +
                 ", code: " + std::to_string(ex.code()));
    } catch (const std::exception& ex) {
        LOG_ERROR("[WGC] std::exception in on_frame_arrived: " + std::string(ex.what()));
    } catch (...) {
        LOG_ERROR("[WGC] Unknown exception in on_frame_arrived");
    }
}

QImage WGCCaptureLoop::copy_texture_to_qimage(ID3D11Texture2D* src)
{
    if (!src) return {};

    D3D11_TEXTURE2D_DESC desc{};
    src->GetDesc(&desc);

    if (desc.Width == 0 || desc.Height == 0) return {};

    // Get shared device
    auto& shared = SharedD3D11Device::instance();
    auto d3d_device = shared.device();
    auto d3d_context = shared.context();
    if (!d3d_device || !d3d_context) return {};

    // Reuse cached staging texture if size matches, otherwise recreate
    if (!cached_staging_texture_ ||
        cached_staging_width_ != desc.Width ||
        cached_staging_height_ != desc.Height) {

        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;

        winrt::com_ptr<ID3D11Texture2D> newStaging;
        if (FAILED(d3d_device->CreateTexture2D(&stagingDesc, nullptr, newStaging.put()))) {
            return {};
        }

        cached_staging_texture_ = newStaging;
        cached_staging_width_ = desc.Width;
        cached_staging_height_ = desc.Height;
    }

    d3d_context->CopyResource(cached_staging_texture_.get(), src);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(d3d_context->Map(cached_staging_texture_.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return {};
    }

    // D3D11 B8G8R8A8 format matches QImage::Format_ARGB32 on little-endian systems
    // No color conversion needed - direct memory copy
    QImage img(desc.Width, desc.Height, QImage::Format_ARGB32);

    const uint8_t* srcBytes = static_cast<const uint8_t*>(mapped.pData);
    const uint32_t rowBytes = desc.Width * 4;

    for (uint32_t y = 0; y < desc.Height; ++y) {
        memcpy(img.scanLine(y), srcBytes + y * mapped.RowPitch, rowBytes);
    }

    d3d_context->Unmap(cached_staging_texture_.get(), 0);
    return img;
}

void WGCCaptureLoop::thread_proc()
{
    // Use multi_threaded apartment for CreateFreeThreaded FramePool
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    LOG_INFO("WGCCaptureLoop thread started");

    if (!init_d3d() || !init_capture_item() || !init_capture_objects()) {
        LOG_ERROR("WGCCaptureLoop: initialization failed");
        return;
    }

    session_.StartCapture();
    LOG_INFO("WGCCaptureLoop: capture started (FreeThreaded mode)");

    // No message loop needed for FreeThreaded FramePool
    // Just wait for stop signal
    while (!stopping_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Cleanup
    try {
        if (session_) session_.Close();
        if (frame_pool_) frame_pool_.Close();
    } catch (...) {
    }

    session_ = nullptr;
    frame_pool_ = nullptr;
    item_ = nullptr;
    swapchain_ = nullptr;

    LOG_INFO("WGCCaptureLoop thread exiting");
}

} // namespace live_assistant
