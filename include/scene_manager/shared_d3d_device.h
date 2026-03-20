#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <mutex>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

namespace live_assistant {

// Shared D3D11 device manager for WGC capture sources.
// Multiple capture sources share the same device to reduce GPU overhead.
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

private:
    SharedD3D11Device() = default;
    ~SharedD3D11Device();

    SharedD3D11Device(const SharedD3D11Device&) = delete;
    SharedD3D11Device& operator=(const SharedD3D11Device&) = delete;

    bool init();

    std::mutex mutex_;
    winrt::com_ptr<ID3D11Device> d3d_device_;
    winrt::com_ptr<ID3D11DeviceContext> d3d_context_;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrt_device_{ nullptr };
};

} // namespace live_assistant