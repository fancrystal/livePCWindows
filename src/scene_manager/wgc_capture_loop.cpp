#include "scene_manager/wgc_capture_loop.h"
#include "common/log.h"

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

    // Make sure we wake the message loop.
    if (worker_.joinable()) {
        PostThreadMessageW(::GetThreadId(static_cast<HANDLE>(worker_.native_handle())), WM_QUIT, 0, 0);
        worker_.join();
    }
}

bool WGCCaptureLoop::init_d3d()
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl;
    winrt::com_ptr<ID3D11Device> dev;
    winrt::com_ptr<ID3D11DeviceContext> ctx;

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   nullptr, 0, D3D11_SDK_VERSION, dev.put(), &fl, ctx.put());
    if (FAILED(hr)) {
        LOG_ERROR("WGCCaptureLoop: D3D11CreateDevice failed");
        return false;
    }

    d3d_device_ = dev;
    d3d_context_ = ctx;
    winrt_device_ = CreateDirect3DDevice(d3d_device_.get());

    return true;
}

bool WGCCaptureLoop::init_capture_item()
{
    auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();

    HRESULT hr = E_FAIL;
    if (cfg_.type == CaptureConfig::TargetType::SCREEN) {
        HMONITOR mon = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
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

    // Match Win32CaptureSample: swapchain + FramePool::Create
    auto dxgiDevice = GetDXGIInterfaceFromObject<ID3D11Device>(winrt_device_);
    dxgiDevice->GetImmediateContext(d3d_context_.put());

    swapchain_ = CreateSwapChain(dxgiDevice.get(), static_cast<uint32_t>(last_size_.Width), static_cast<uint32_t>(last_size_.Height),
                                 static_cast<DXGI_FORMAT>(pixel_format_), 2);

    frame_pool_ = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::Create(winrt_device_, pixel_format_, 2, last_size_);
    session_ = frame_pool_.CreateCaptureSession(item_);
    session_.IsCursorCaptureEnabled(cfg_.capture_cursor);
    session_.IsBorderRequired(cfg_.capture_border);

    frame_pool_.FrameArrived({ this, &WGCCaptureLoop::on_frame_arrived });

    return true;
}

void WGCCaptureLoop::resize_swapchain()
{
    if (!swapchain_) return;
    winrt::check_hresult(swapchain_->ResizeBuffers(2,
        static_cast<uint32_t>(last_size_.Width),
        static_cast<uint32_t>(last_size_.Height),
        static_cast<DXGI_FORMAT>(pixel_format_),
        0));
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
    LOG_INFO("[DIAG] WGCCaptureLoop::on_frame_arrived - 帧到达，stopping_: " + std::string(stopping_ ? "true" : "false"));
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
        d3d_context_->CopyResource(backBuffer.get(), surfaceTexture.get());
    }

    DXGI_PRESENT_PARAMETERS params{};
    swapchain_->Present1(1, 0, &params);

    if (resized) {
        frame_pool_.Recreate(winrt_device_, pixel_format_, 2, last_size_);
        return;
    }

    // Read back into QImage (additional step for Qt pipeline)
    if (surfaceTexture) {
        LOG_INFO("[DIAG] WGCCaptureLoop::on_frame_arrived - 开始复制纹理到QImage");
        QImage img = copy_texture_to_qimage(surfaceTexture.get());
        LOG_INFO("[DIAG] WGCCaptureLoop::on_frame_arrived - QImage创建完成，isNull: " + std::string(img.isNull() ? "true" : "false") +
                 ", 尺寸: " + std::to_string(img.width()) + "x" + std::to_string(img.height()));

        if (!img.isNull()) {
            ImageCallback cb;
            {
                std::lock_guard<std::mutex> lk(cb_mutex_);
                cb = cb_;
            }
            LOG_INFO("[DIAG] WGCCaptureLoop::on_frame_arrived - 回调函数存在: " + std::string(cb ? "true" : "false"));
            if (cb) {
                LOG_INFO("[DIAG] WGCCaptureLoop::on_frame_arrived - 调用回调函数");
                cb(img);
            } else {
                LOG_WARNING("[DIAG] WGCCaptureLoop::on_frame_arrived - 回调函数为空");
            }
        } else {
            LOG_WARNING("[DIAG] WGCCaptureLoop::on_frame_arrived - QImage为空，跳过回调");
        }
    } else {
        LOG_WARNING("[DIAG] WGCCaptureLoop::on_frame_arrived - surfaceTexture为空");
    }
}

QImage WGCCaptureLoop::copy_texture_to_qimage(ID3D11Texture2D* src)
{
    if (!src) return {};

    D3D11_TEXTURE2D_DESC desc{};
    src->GetDesc(&desc);

    if (desc.Width == 0 || desc.Height == 0) return {};

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    winrt::com_ptr<ID3D11Texture2D> staging;
    if (FAILED(d3d_device_->CreateTexture2D(&stagingDesc, nullptr, staging.put()))) {
        return {};
    }

    d3d_context_->CopyResource(staging.get(), src);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(d3d_context_->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return {};
    }

    // Surface is BGRA8 by default. Convert to RGBA8888 for Qt.
    QImage img(desc.Width, desc.Height, QImage::Format_RGBA8888);

    const uint8_t* srcBytes = static_cast<const uint8_t*>(mapped.pData);
    for (uint32_t y = 0; y < desc.Height; ++y) {
        const uint8_t* row = srcBytes + y * mapped.RowPitch;
        uint8_t* out = img.scanLine(y);
        for (uint32_t x = 0; x < desc.Width; ++x) {
            uint8_t b = row[x * 4 + 0];
            uint8_t g = row[x * 4 + 1];
            uint8_t r = row[x * 4 + 2];
            uint8_t a = row[x * 4 + 3];
            out[x * 4 + 0] = r;
            out[x * 4 + 1] = g;
            out[x * 4 + 2] = b;
            out[x * 4 + 3] = a;
        }
    }

    d3d_context_->Unmap(staging.get(), 0);
    return img;
}

void WGCCaptureLoop::thread_proc()
{
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    // Win32CaptureSample requires a DispatcherQueue when using FramePool::Create
    // We'll create a hidden message window by running a message pump.
    LOG_INFO("WGCCaptureLoop thread started");

    if (!init_d3d() || !init_capture_item() || !init_capture_objects()) {
        LOG_ERROR("WGCCaptureLoop: initialization failed");
        return;
    }

    session_.StartCapture();
    LOG_INFO("WGCCaptureLoop: capture started");

    MSG msg;
    while (!stopping_ && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
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
