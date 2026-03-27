#pragma once

#include "scene_manager/gpu_texture_ref.h"
#include <d3d11.h>
#include <winrt/base.h>
#include <cstdint>

namespace live_assistant {

// GPU 色彩转换器：使用 D3D11VideoProcessor 将 BGRA 纹理转换为 NV12 纹理。
//
// 为什么选 D3D11VideoProcessor 而非 Compute Shader：
//   - 原生支持输出 DXGI_FORMAT_NV12
//   - 使用 GPU 固定功能 Video Processing Unit，不占 3D 管线
//   - FL9.1+ 全平台支持
//   - 代码量更少，精度符合 BT.601/709 标准
//
// 输出的 NV12 纹理可直接被 FFmpeg D3D11VA 帧上下文引用（Phase 4）。
class GpuColorConverter {
public:
    GpuColorConverter();
    ~GpuColorConverter();

    // 初始化 VideoProcessor
    // input_w/h:  输入 BGRA 纹理的分辨率（通常与画布相同）
    // output_w/h: 编码器输入分辨率（允许同时缩放）
    bool initialize(int input_w, int input_h,
                    int output_w, int output_h,
                    DXGI_FORMAT input_format = DXGI_FORMAT_B8G8R8A8_UNORM);

    // 执行 BGRA → NV12 转换
    // input_bgra: 来自 GpuCompositor 的输出纹理
    // 返回 NV12 GpuTextureRef（与内部输出纹理共享，有效至下次 convert 调用）
    GpuTextureRef convert(ID3D11Texture2D* input_bgra);

    // 直接获取内部 NV12 纹理（不增加引用，调用者不应持有超过下次 convert）
    ID3D11Texture2D* nv12_texture() const { return nv12_texture_.get(); }

    bool is_initialized() const { return initialized_; }
    void shutdown();

private:
    bool create_nv12_texture(int w, int h);
    bool create_output_view();

    bool initialized_  = false;
    int  input_width_  = 0, input_height_  = 0;
    int  output_width_ = 0, output_height_ = 0;
    DXGI_FORMAT input_format_ = DXGI_FORMAT_B8G8R8A8_UNORM;

    // 借用自 SharedD3D11Device，生命周期由其管理，不持有额外引用
    ID3D11VideoDevice*  video_device_  = nullptr;
    ID3D11VideoContext* video_context_ = nullptr;

    winrt::com_ptr<ID3D11VideoProcessorEnumerator>   enumerator_;
    winrt::com_ptr<ID3D11VideoProcessor>             processor_;

    // NV12 输出纹理 + 输出视图（固定一个，每次 convert 重用）
    winrt::com_ptr<ID3D11Texture2D>                  nv12_texture_;
    winrt::com_ptr<ID3D11VideoProcessorOutputView>   output_view_;
};

} // namespace live_assistant
