#pragma once

#include "scene_manager/gpu_texture_ref.h"
#include <d3d11.h>
#include <winrt/base.h>
#include <vector>
#include <string>
#include <mutex>
#include <cstdint>
#include <unordered_map>
#include <algorithm>

namespace live_assistant {

// 单个图层描述，供 GpuCompositor::compose() 使用
struct GpuCompositorLayer {
    std::string source_id;
    ID3D11Texture2D*          texture  = nullptr;  // 原始纹理（borrowed，由外部管理生命周期）
    ID3D11ShaderResourceView* srv      = nullptr;  // 对应 SRV（由 GpuCompositor 内部管理）
    // 画布坐标系中的目标矩形（与 CompositorLayer::dest_rect 对应）
    float dest_x = 0.f, dest_y = 0.f;
    float dest_w = 0.f, dest_h = 0.f;
    float opacity = 1.f;
    bool  visible = true;
    int   z_order = 0;
};

// D3D11 GPU 合成器
// 用全屏四边形 + 像素着色器替代 QPainter CPU 合成，
// 输出 BGRA Render Target 纹理，供 GpuColorConverter 和 QOpenGLWidget 使用。
class GpuCompositor {
public:
    GpuCompositor();
    ~GpuCompositor();

    // 初始化 GPU 资源（着色器、采样器、混合状态）
    // 必须在 SharedD3D11Device 有效后调用
    bool initialize(int canvas_width, int canvas_height);

    // 调整画布分辨率，重建 Render Target
    bool resize(int canvas_width, int canvas_height);

    // 执行一次合成：按 z_order 绘制所有可见图层到内部 Render Target
    // 返回合成结果的 GpuTextureRef（与内部 RT 纹理共享，有效至下次 compose 调用）
    GpuTextureRef compose(const std::vector<GpuCompositorLayer>& layers);

    // 获取最近一次 compose 的结果 SRV（供 QOpenGLWidget / Phase 5 使用）
    ID3D11ShaderResourceView* output_srv() const { return rt_srv_.get(); }
    ID3D11Texture2D*          output_texture() const { return rt_texture_.get(); }

    int canvas_width()  const { return canvas_width_; }
    int canvas_height() const { return canvas_height_; }

    bool is_initialized() const { return initialized_; }

    void shutdown();

private:
    bool create_render_target(int width, int height);
    bool create_shaders();
    bool create_sampler_and_blend();
    bool create_vertex_buffer();

    void draw_quad(ID3D11ShaderResourceView* srv,
                   float x, float y, float w, float h,
                   float opacity);

    bool initialized_  = false;
    int  canvas_width_ = 0;
    int  canvas_height_ = 0;

    // Render Target（合成输出，BGRA）
    winrt::com_ptr<ID3D11Texture2D>          rt_texture_;
    winrt::com_ptr<ID3D11RenderTargetView>   rt_view_;
    winrt::com_ptr<ID3D11ShaderResourceView> rt_srv_;

    // 着色器
    winrt::com_ptr<ID3D11VertexShader> vs_;
    winrt::com_ptr<ID3D11PixelShader>  ps_;
    winrt::com_ptr<ID3D11InputLayout>  input_layout_;

    // 动态顶点缓冲区（每帧每层更新一次）
    winrt::com_ptr<ID3D11Buffer> vb_;

    // 常量缓冲区（opacity）
    winrt::com_ptr<ID3D11Buffer> cb_;

    // 采样器 / 混合状态
    winrt::com_ptr<ID3D11SamplerState>  sampler_;
    winrt::com_ptr<ID3D11BlendState>    blend_state_;

    // SRV 缓存（避免每帧重建，按纹理指针索引）
    // key: ID3D11Texture2D*, value: SRV
    std::unordered_map<ID3D11Texture2D*, winrt::com_ptr<ID3D11ShaderResourceView>> srv_cache_;

    D3D11_VIEWPORT viewport_{};
};

} // namespace live_assistant
