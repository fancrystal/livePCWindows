#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <memory>

namespace live_assistant {

class SharedD3D11Device;
struct VideoFrame;

// CsBgraToNv12
// ---------------------------------------------------------------------------
// Compute-shader based BGRA → NV12 color conversion + GPU→CPU readback.
//
// 设计目的：
//   GpuCompositor 已经输出 BGRA Render Target，但 H264 编码器需要 NV12。
//   传统做法是用 D3D11VideoProcessor，但 Intel iGPU 的 VideoProcessor 输出
//   会被打上 RC/CCS 标志（Render Compressed），CopySubresourceRegion 会崩。
//
//   该类用 Compute Shader 直接读 BGRA SRV、写 R8/R8G8 UAV：
//     - CS 通过 SRV 读 BGRA，驱动内部透明解压（避免 RC 崩溃）
//     - UAV 写入的 R8/R8G8 纹理不会被驱动打 RC 标志
//     - 跨厂商通用（Intel / NVIDIA / AMD 都支持 D3D11 CS 5.0）
//
// 颜色空间：BT.601 limited range（与 FFmpeg sws_scale 默认输出一致，
// 编码器无需额外指定 color_range 即可正确解码）
//
// 输出：CPU 内存 NV12 VideoFrame（与既有 CPU 路径同构，编码器无需感知差异）
//
// 注意：本类不负责管理 GpuCompositor 的 BGRA 纹理生命周期，
// 调用方传入的 SRV 必须在 dispatch() 调用期间保持有效。
class CsBgraToNv12 {
public:
    CsBgraToNv12();
    ~CsBgraToNv12();

    CsBgraToNv12(const CsBgraToNv12&)            = delete;
    CsBgraToNv12& operator=(const CsBgraToNv12&) = delete;

    // 初始化：编译 CS、分配 UAV / STAGING 纹理。
    // 尺寸必须为偶数（NV12 要求）。失败返回 false。
    bool initialize(int width, int height);

    // 释放所有 GPU 资源。
    void shutdown();

    // 主转换接口：BGRA SRV → NV12 VideoFrame
    //   bgra_srv: GpuCompositor 输出的 BGRA Shader Resource View
    //   timestamp_ms: 写入 VideoFrame::timestamp_ms
    // 失败返回 nullptr。
    std::shared_ptr<VideoFrame> convert_to_cpu(
        ID3D11ShaderResourceView* bgra_srv,
        int64_t timestamp_ms);

    // 仅做 GPU 转换（不回读），结果保留在内部 UAV 纹理中。
    // 供未来零拷贝路径（QSV / NVENC D3D11VA 直接喂入）使用。
    bool dispatch_only(ID3D11ShaderResourceView* bgra_srv);

    bool is_initialized() const { return initialized_; }

    int width()  const { return width_;  }
    int height() const { return height_; }

private:
    bool create_compute_shader();
    bool create_textures(int width, int height);

    Microsoft::WRL::ComPtr<ID3D11ComputeShader> cs_;

    // UAV 输出纹理（GPU 内部）
    Microsoft::WRL::ComPtr<ID3D11Texture2D>             uav_y_texture_;   // W x H,     R8_UNORM
    Microsoft::WRL::ComPtr<ID3D11Texture2D>             uav_uv_texture_;  // W/2 x H/2, R8G8_UNORM
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>   uav_y_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>   uav_uv_;

    // STAGING 纹理（GPU → CPU 回读）
    Microsoft::WRL::ComPtr<ID3D11Texture2D>             staging_y_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>             staging_uv_;

    int  width_       = 0;
    int  height_      = 0;
    bool initialized_ = false;
};

} // namespace live_assistant
