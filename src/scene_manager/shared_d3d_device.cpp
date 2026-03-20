#include "scene_manager/shared_d3d_device.h"
#include "common/log.h"

#include <windows.graphics.directx.direct3d11.interop.h>

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

} // namespace live_assistant