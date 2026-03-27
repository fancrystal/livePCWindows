#pragma once

#include <d3d11.h>
#include <winrt/base.h>
#include <cstdint>

namespace live_assistant {

// 轻量级 GPU 纹理引用，持有 COM 引用计数保证生命周期。
// 用于 WGC 捕获 → GPU 合成器的零拷贝纹理传递。
// 每帧创建新实例，旧实例析构时自动释放 COM 引用。
struct GpuTextureRef {
    winrt::com_ptr<ID3D11Texture2D> texture;   // COM 引用（保证生命周期）
    uint32_t width  = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    int64_t  timestamp_ms = 0;
    uint64_t frame_id     = 0;   // 单调递增，供去重使用

    bool is_valid() const {
        return texture && width > 0 && height > 0;
    }
};

} // namespace live_assistant
