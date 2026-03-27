#include "scene_manager/shared_d3d_device.h"
#include "common/log.h"

#include <windows.graphics.directx.direct3d11.interop.h>
#include <string>
#include <QImage>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace live_assistant {

SharedD3D11Device& SharedD3D11Device::instance()
{
    static SharedD3D11Device inst;
    return inst;
}

SharedD3D11Device::~SharedD3D11Device()
{
    winrt_device_ = nullptr;
    d3d_context_ = nullptr;
    d3d_device_ = nullptr;
}

bool SharedD3D11Device::init()
{
    if (d3d_device_) return true;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl;

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        d3d_device_.put(),
        &fl,
        d3d_context_.put());

    if (FAILED(hr)) {
        LOG_ERROR("SharedD3D11Device: D3D11CreateDevice failed");
        return false;
    }

    // Create WinRT wrapped device
    winrt::com_ptr<IDXGIDevice> dxgi;
    hr = d3d_device_->QueryInterface(dxgi.put());
    if (FAILED(hr)) {
        LOG_ERROR("SharedD3D11Device: QueryInterface IDXGIDevice failed");
        return false;
    }

    winrt::com_ptr<IInspectable> insp;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put());
    if (FAILED(hr)) {
        LOG_ERROR("SharedD3D11Device: CreateDirect3D11DeviceFromDXGIDevice failed");
        return false;
    }

    winrt_device_ = insp.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

    // 设备创建后立即内联启用多线程保护（不能调用 enable_multithread_protection()，
    // 因为调用方可能已经持有 mutex_，重复 lock 非递归互斥锁会触发 std::system_error）
    if (!multithread_enabled_) {
        auto mt = d3d_context_.try_as<ID3D11Multithread>();
        if (mt) {
            mt->SetMultithreadProtected(TRUE);
            multithread_enabled_ = true;
            LOG_INFO("SharedD3D11Device: multithread protection enabled (inline init)");
        }
    }

    LOG_INFO("SharedD3D11Device: initialized successfully");
    return true;
}

ID3D11Device* SharedD3D11Device::device()
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (!d3d_device_) {
        init();
    }
    return d3d_device_.get();
}

ID3D11DeviceContext* SharedD3D11Device::context()
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (!d3d_context_) {
        init();
    }
    return d3d_context_.get();
}

winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice SharedD3D11Device::winrt_device()
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (!winrt_device_) {
        init();
    }
    return winrt_device_;
}

// ---------------------------------------------------------------------------
// Phase 0: multithread protection
// ---------------------------------------------------------------------------

bool SharedD3D11Device::enable_multithread_protection()
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (multithread_enabled_) return true;
    // 注意：不能在持锁时调用 init()（init() 内部也会操作此对象的内联保护逻辑）
    // 若设备尚未初始化，则通过外部调用 device()/context() 先触发初始化
    if (!d3d_context_) {
        LOG_WARNING("SharedD3D11Device: enable_multithread_protection called before init, skip");
        return false;
    }

    auto mt = d3d_context_.try_as<ID3D11Multithread>();
    if (!mt) {
        LOG_WARNING("SharedD3D11Device: ID3D11Multithread not available, skipping");
        return false;
    }
    mt->SetMultithreadProtected(TRUE);
    multithread_enabled_ = true;
    LOG_INFO("SharedD3D11Device: multithread protection enabled");
    return true;
}

// ---------------------------------------------------------------------------
// Phase 0: Video Processing interfaces
// ---------------------------------------------------------------------------

ID3D11VideoDevice* SharedD3D11Device::video_device()
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (!d3d_device_ && !init()) return nullptr;
    if (!video_device_) {
        video_device_ = d3d_device_.try_as<ID3D11VideoDevice>();
        if (!video_device_) {
            LOG_WARNING("SharedD3D11Device: ID3D11VideoDevice not supported by this GPU");
            return nullptr;
        }
        LOG_INFO("SharedD3D11Device: ID3D11VideoDevice acquired");
    }
    return video_device_.get();
}

ID3D11VideoContext* SharedD3D11Device::video_context()
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (!d3d_context_ && !init()) return nullptr;
    if (!video_context_) {
        video_context_ = d3d_context_.try_as<ID3D11VideoContext>();
        if (!video_context_) {
            LOG_WARNING("SharedD3D11Device: ID3D11VideoContext not supported by this GPU");
            return nullptr;
        }
        LOG_INFO("SharedD3D11Device: ID3D11VideoContext acquired");
    }
    return video_context_.get();
}

// ---------------------------------------------------------------------------
// Phase 0: texture / view creation helpers
// ---------------------------------------------------------------------------

winrt::com_ptr<ID3D11Texture2D> SharedD3D11Device::create_texture_2d(
    uint32_t width, uint32_t height,
    DXGI_FORMAT format,
    D3D11_USAGE usage,
    UINT bind_flags,
    UINT cpu_access_flags,
    UINT misc_flags)
{
    if (!device()) return {};   // triggers init() internally

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width              = width;
    desc.Height             = height;
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = format;
    desc.SampleDesc.Count   = 1;
    desc.Usage              = usage;
    desc.BindFlags          = bind_flags;
    desc.CPUAccessFlags     = cpu_access_flags;
    desc.MiscFlags          = misc_flags;

    winrt::com_ptr<ID3D11Texture2D> tex;
    HRESULT hr = d3d_device_->CreateTexture2D(&desc, nullptr, tex.put());
    if (FAILED(hr)) {
        LOG_ERROR("SharedD3D11Device::create_texture_2d failed: " +
                  std::to_string(width) + "x" + std::to_string(height) +
                  " fmt=" + std::to_string(static_cast<int>(format)) +
                  " hr=" + std::to_string(hr));
        return {};
    }
    return tex;
}

winrt::com_ptr<ID3D11ShaderResourceView> SharedD3D11Device::create_srv(
    ID3D11Texture2D* texture, DXGI_FORMAT format)
{
    if (!texture || !device()) return {};

    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format                    = format;
    desc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipLevels       = 1;
    desc.Texture2D.MostDetailedMip = 0;

    winrt::com_ptr<ID3D11ShaderResourceView> srv;
    HRESULT hr = d3d_device_->CreateShaderResourceView(texture, &desc, srv.put());
    if (FAILED(hr)) {
        LOG_ERROR("SharedD3D11Device::create_srv failed: hr=" + std::to_string(hr));
        return {};
    }
    return srv;
}

winrt::com_ptr<ID3D11RenderTargetView> SharedD3D11Device::create_rtv(
    ID3D11Texture2D* texture, DXGI_FORMAT format)
{
    if (!texture || !device()) return {};

    D3D11_RENDER_TARGET_VIEW_DESC desc{};
    desc.Format             = format;
    desc.ViewDimension      = D3D11_RTV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipSlice = 0;

    winrt::com_ptr<ID3D11RenderTargetView> rtv;
    HRESULT hr = d3d_device_->CreateRenderTargetView(texture, &desc, rtv.put());
    if (FAILED(hr)) {
        LOG_ERROR("SharedD3D11Device::create_rtv failed: hr=" + std::to_string(hr));
        return {};
    }
    return rtv;
}

// ---------------------------------------------------------------------------
// Phase 1b: CPU image → GPU texture upload
// ---------------------------------------------------------------------------

winrt::com_ptr<ID3D11Texture2D> SharedD3D11Device::upload_image_to_texture(
    const QImage& image, ID3D11Texture2D* reuse)
{
    if (image.isNull()) return {};
    if (!device()) return {};

    // 统一转换为 BGRA（QImage::Format_ARGB32 在小端系统即 BGRA 布局，与 D3D11 B8G8R8A8 一致）
    QImage src = (image.format() == QImage::Format_ARGB32)
                 ? image
                 : image.convertToFormat(QImage::Format_ARGB32);

    const uint32_t w = static_cast<uint32_t>(src.width());
    const uint32_t h = static_cast<uint32_t>(src.height());
    if (w == 0 || h == 0) return {};

    winrt::com_ptr<ID3D11Texture2D> tex;

    // 复用已有纹理（尺寸相同时避免重新分配）
    if (reuse) {
        D3D11_TEXTURE2D_DESC existing{};
        reuse->GetDesc(&existing);
        if (existing.Width == w && existing.Height == h &&
            existing.Usage == D3D11_USAGE_DYNAMIC) {
            tex.attach(reuse);
            reuse->AddRef();   // attach 不增加引用，补一次
        }
    }

    if (!tex) {
        // 新建 DYNAMIC 纹理：MAP_WRITE_DISCARD 是驱动优化的上传路径
        tex = create_texture_2d(w, h,
                                DXGI_FORMAT_B8G8R8A8_UNORM,
                                D3D11_USAGE_DYNAMIC,
                                D3D11_BIND_SHADER_RESOURCE,
                                D3D11_CPU_ACCESS_WRITE, 0);
        if (!tex) return {};
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = d3d_context_->Map(tex.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        LOG_ERROR("SharedD3D11Device::upload_image_to_texture: Map failed hr=" +
                  std::to_string(hr));
        return {};
    }

    const uint32_t row_bytes = w * 4;
    for (uint32_t y = 0; y < h; ++y) {
        memcpy(static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch,
               src.constScanLine(y), row_bytes);
    }

    d3d_context_->Unmap(tex.get(), 0);
    return tex;
}

} // namespace live_assistant