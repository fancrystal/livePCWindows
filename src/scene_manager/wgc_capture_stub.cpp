// Lightweight stub to validate WGC plumbing during development.
// This file provides a small helper that can be called to test whether
// we can create a capture item and get a single frame. It will be
// extended into a full CaptureSource implementation in follow-ups.

// Lightweight WGC availability stub: do not depend on C++/WinRT here.
#include "common/log.h"
#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <windows.graphics.capture.interop.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowsapp.lib")

namespace live_assistant {

bool try_create_wgc_preview_for_window(HWND hwnd) {
    // Basic checks: window valid and visible
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        LOG_INFO("WGC stub: Window not valid or not visible");
        return false;
    }

    // Check Windows version (need 1903+ for WGC)
    typedef LONG(WINAPI* RtlGetVersionFunc)(RTL_OSVERSIONINFOW* lpVersionInformation);
    HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
    if (hNtDll) {
        auto pRtlGetVersion = (RtlGetVersionFunc)GetProcAddress(hNtDll, "RtlGetVersion");
        if (pRtlGetVersion) {
            RTL_OSVERSIONINFOW osvi = { sizeof(osvi) };
            if (pRtlGetVersion(&osvi) == 0) {
                // Need Windows 10 build 18362 (19H1) or higher
                if (osvi.dwMajorVersion < 10 ||
                    (osvi.dwMajorVersion == 10 && osvi.dwBuildNumber < 18362)) {
                    LOG_INFO("WGC stub: Windows version too old for WGC (need 1903+)");
                    return false;
                }
            }
        }
    }

    // Try to create D3D11 device (required for WGC)
    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &d3dDevice,
        &featureLevel,
        &d3dContext);

    if (FAILED(hr)) {
        LOG_INFO("WGC stub: Failed to create D3D11 device");
        return false;
    }

    // Try to create GraphicsCaptureItem for the window
    try {
        auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                                                     IGraphicsCaptureItemInterop>();

        winrt::Windows::Graphics::Capture::GraphicsCaptureItem item = nullptr;
        hr = interop->CreateForWindow(hwnd,
                                      winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                                      winrt::put_abi(item));

        if (SUCCEEDED(hr) && item) {
            LOG_INFO("WGC stub: Successfully created GraphicsCaptureItem");
            // Cleanup
            item = nullptr;
            d3dContext->Release();
            d3dDevice->Release();
            return true;
        } else {
            LOG_INFO("WGC stub: Failed to create GraphicsCaptureItem");
        }
    }
    catch (const winrt::hresult_error& ex) {
        LOG_INFO("WGC stub: WinRT error creating GraphicsCaptureItem: " +
                 winrt::to_string(ex.message()));
    }
    catch (...) {
        LOG_INFO("WGC stub: Exception creating GraphicsCaptureItem");
    }

    // Cleanup
    d3dContext->Release();
    d3dDevice->Release();
    return false;
}

} // namespace live_assistant

