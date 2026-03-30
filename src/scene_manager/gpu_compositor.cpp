#include "scene_manager/gpu_compositor.h"
#include "scene_manager/shared_d3d_device.h"
#include "common/log.h"

#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <algorithm>
#include <string>
#include <unordered_set>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d11.lib")

namespace live_assistant {

// ─── HLSL 着色器（运行时编译，避免 .cso 文件依赖）─────────────────────────────

static const char* kVertexShaderHLSL = R"HLSL(
struct VS_INPUT {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
};
struct VS_OUTPUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};
VS_OUTPUT main(VS_INPUT i) {
    VS_OUTPUT o;
    o.pos = float4(i.pos, 0.0, 1.0);
    o.uv  = i.uv;
    return o;
}
)HLSL";

static const char* kPixelShaderHLSL = R"HLSL(
Texture2D    tex     : register(t0);
SamplerState smp     : register(s0);
cbuffer PerLayer : register(b0) {
    float opacity;
    float3 _pad;
};
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 c = tex.Sample(smp, uv);
    c.a *= opacity;
    return c;
}
)HLSL";

// ─── 顶点结构 ────────────────────────────────────────────────────────────────

struct QuadVertex {
    float x, y;   // NDC 坐标
    float u, v;   // UV
};

struct CBPerLayer {
    float opacity;
    float pad[3];
};

// ─── GpuCompositor ───────────────────────────────────────────────────────────

GpuCompositor::GpuCompositor() = default;
GpuCompositor::~GpuCompositor() { shutdown(); }

bool GpuCompositor::initialize(int canvas_width, int canvas_height)
{
    if (initialized_) return true;

    auto& shared = SharedD3D11Device::instance();
    // device() 会触发 SharedD3D11Device::init()（懒初始化），is_valid() 不触发初始化
    // 若在 D3D 设备创建前调用 initialize()，is_valid() 会错误地返回 false 导致 GPU 路径被禁用
    if (!shared.device()) {
        LOG_ERROR("[GpuCompositor] SharedD3D11Device initialization failed");
        return false;
    }

    if (!create_shaders()) return false;
    if (!create_sampler_and_blend()) return false;
    if (!create_vertex_buffer()) return false;
    if (!create_render_target(canvas_width, canvas_height)) return false;

    canvas_width_  = canvas_width;
    canvas_height_ = canvas_height;
    initialized_   = true;

    LOG_INFO("[GpuCompositor] initialized: " +
             std::to_string(canvas_width) + "x" + std::to_string(canvas_height));
    return true;
}

bool GpuCompositor::resize(int w, int h)
{
    if (!initialized_ || (w == canvas_width_ && h == canvas_height_)) return true;

    // 先释放旧的 RT 资源
    rt_srv_.detach(); rt_srv_ = nullptr;
    rt_view_.detach(); rt_view_ = nullptr;
    rt_texture_.detach(); rt_texture_ = nullptr;

    if (!create_render_target(w, h)) return false;
    canvas_width_  = w;
    canvas_height_ = h;
    LOG_INFO("[GpuCompositor] resized to: " + std::to_string(w) + "x" + std::to_string(h));
    return true;
}

GpuTextureRef GpuCompositor::compose(const std::vector<GpuCompositorLayer>& layers)
{
    if (!initialized_) return {};

    auto& shared  = SharedD3D11Device::instance();
    auto* ctx     = shared.context();
    if (!ctx) return {};

    // 1. 设置 Render Target
    ID3D11RenderTargetView* rtv = rt_view_.get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->RSSetViewports(1, &viewport_);

    // 2. 清除为不透明黑色
    const float clear[4] = { 0.f, 0.f, 0.f, 1.f };
    ctx->ClearRenderTargetView(rtv, clear);

    // 3. 设置公共管线状态
    ctx->VSSetShader(vs_.get(), nullptr, 0);
    ctx->PSSetShader(ps_.get(), nullptr, 0);
    ID3D11SamplerState* smp = sampler_.get();
    ctx->PSSetSamplers(0, 1, &smp);
    ctx->OMSetBlendState(blend_state_.get(), nullptr, 0xFFFFFFFF);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->IASetInputLayout(input_layout_.get());

    // 4. 按 z_order 排序（值越小越先绘制，越靠底层）
    std::vector<const GpuCompositorLayer*> sorted;
    sorted.reserve(layers.size());
    for (const auto& l : layers) {
        if (l.visible && l.texture) sorted.push_back(&l);
    }

    // 清理 SRV 缓存中已失效的条目（纹理指针不在当前帧的图层列表中）
    // 防止纹理销毁后新纹理分配到相同地址导致返回旧 SRV
    {
        std::unordered_set<ID3D11Texture2D*> active_textures;
        active_textures.reserve(sorted.size());
        for (const auto* l : sorted) active_textures.insert(l->texture);
        for (auto it = srv_cache_.begin(); it != srv_cache_.end(); ) {
            if (active_textures.find(it->first) == active_textures.end()) {
                it = srv_cache_.erase(it);
            } else {
                ++it;
            }
        }
    }
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const GpuCompositorLayer* a, const GpuCompositorLayer* b) {
                         return a->z_order < b->z_order;
                     });

    // 5. 逐层绘制
    for (const auto* layer : sorted) {
        // 获取或创建 SRV（按纹理指针缓存）
        auto it = srv_cache_.find(layer->texture);
        if (it == srv_cache_.end()) {
            auto srv = shared.create_srv(layer->texture, DXGI_FORMAT_B8G8R8A8_UNORM);
            if (!srv) {
                LOG_WARNING("[GpuCompositor] failed to create SRV for layer " + layer->source_id);
                continue;
            }
            it = srv_cache_.emplace(layer->texture, std::move(srv)).first;
        }

        draw_quad(it->second.get(),
                  layer->dest_x, layer->dest_y, layer->dest_w, layer->dest_h,
                  layer->opacity);
    }

    // 6. 解绑 RT，避免后续资源冲突
    ID3D11RenderTargetView* null_rtv = nullptr;
    ctx->OMSetRenderTargets(1, &null_rtv, nullptr);

    // 7. 返回结果纹理引用
    GpuTextureRef result;
    result.texture   = rt_texture_;
    result.width     = static_cast<uint32_t>(canvas_width_);
    result.height    = static_cast<uint32_t>(canvas_height_);
    result.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    return result;
}

void GpuCompositor::shutdown()
{
    if (!initialized_) return;
    srv_cache_.clear();
    rt_srv_ = nullptr;
    rt_view_ = nullptr;
    rt_texture_ = nullptr;
    vs_ = nullptr;
    ps_ = nullptr;
    input_layout_ = nullptr;
    vb_ = nullptr;
    cb_ = nullptr;
    sampler_ = nullptr;
    blend_state_ = nullptr;
    initialized_ = false;
    LOG_INFO("[GpuCompositor] shutdown");
}

// ─── 私有：创建 Render Target ─────────────────────────────────────────────

bool GpuCompositor::create_render_target(int width, int height)
{
    auto& shared = SharedD3D11Device::instance();

    rt_texture_ = shared.create_texture_2d(
        static_cast<uint32_t>(width), static_cast<uint32_t>(height),
        DXGI_FORMAT_B8G8R8A8_UNORM,
        D3D11_USAGE_DEFAULT,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
        0, 0);
    if (!rt_texture_) {
        LOG_ERROR("[GpuCompositor] failed to create RT texture");
        return false;
    }

    rt_view_ = shared.create_rtv(rt_texture_.get(), DXGI_FORMAT_B8G8R8A8_UNORM);
    if (!rt_view_) {
        LOG_ERROR("[GpuCompositor] failed to create RTV");
        return false;
    }

    rt_srv_ = shared.create_srv(rt_texture_.get(), DXGI_FORMAT_B8G8R8A8_UNORM);
    if (!rt_srv_) {
        LOG_ERROR("[GpuCompositor] failed to create RT SRV");
        return false;
    }

    viewport_ = D3D11_VIEWPORT{
        0.f, 0.f,
        static_cast<float>(width), static_cast<float>(height),
        0.f, 1.f
    };
    return true;
}

// ─── 私有：编译 + 创建着色器 ────────────────────────────────────────────────

bool GpuCompositor::create_shaders()
{
    auto* device = SharedD3D11Device::instance().device();

    // 编译 Vertex Shader
    winrt::com_ptr<ID3DBlob> vs_blob, err_blob;
    HRESULT hr = D3DCompile(
        kVertexShaderHLSL, strlen(kVertexShaderHLSL),
        "VS", nullptr, nullptr, "main", "vs_4_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        vs_blob.put(), err_blob.put());
    if (FAILED(hr)) {
        std::string err = err_blob ? static_cast<const char*>(err_blob->GetBufferPointer()) : "unknown";
        LOG_ERROR("[GpuCompositor] VS compile failed: " + err);
        return false;
    }

    hr = device->CreateVertexShader(
        vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
        nullptr, vs_.put());
    if (FAILED(hr)) {
        LOG_ERROR("[GpuCompositor] CreateVertexShader failed");
        return false;
    }

    // 创建 Input Layout（POSITION float2 + TEXCOORD0 float2）
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = device->CreateInputLayout(
        layout, 2,
        vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
        input_layout_.put());
    if (FAILED(hr)) {
        LOG_ERROR("[GpuCompositor] CreateInputLayout failed");
        return false;
    }

    // 编译 Pixel Shader
    winrt::com_ptr<ID3DBlob> ps_blob;
    hr = D3DCompile(
        kPixelShaderHLSL, strlen(kPixelShaderHLSL),
        "PS", nullptr, nullptr, "main", "ps_4_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        ps_blob.put(), err_blob.put());
    if (FAILED(hr)) {
        std::string err = err_blob ? static_cast<const char*>(err_blob->GetBufferPointer()) : "unknown";
        LOG_ERROR("[GpuCompositor] PS compile failed: " + err);
        return false;
    }

    hr = device->CreatePixelShader(
        ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
        nullptr, ps_.put());
    if (FAILED(hr)) {
        LOG_ERROR("[GpuCompositor] CreatePixelShader failed");
        return false;
    }

    // 常量缓冲区（opacity）
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth      = sizeof(CBPerLayer);
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cbd, nullptr, cb_.put());
    if (FAILED(hr)) {
        LOG_ERROR("[GpuCompositor] CreateBuffer (CB) failed");
        return false;
    }

    LOG_INFO("[GpuCompositor] shaders compiled successfully");
    return true;
}

// ─── 私有：采样器 + 混合状态 ────────────────────────────────────────────────

bool GpuCompositor::create_sampler_and_blend()
{
    auto* device = SharedD3D11Device::instance().device();

    // 线性过滤，Clamp
    D3D11_SAMPLER_DESC sd{};
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;

    HRESULT hr = device->CreateSamplerState(&sd, sampler_.put());
    if (FAILED(hr)) {
        LOG_ERROR("[GpuCompositor] CreateSamplerState failed");
        return false;
    }

    // Alpha 混合：SrcAlpha / InvSrcAlpha
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend             = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend            = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp              = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha        = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha       = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha         = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = device->CreateBlendState(&bd, blend_state_.put());
    if (FAILED(hr)) {
        LOG_ERROR("[GpuCompositor] CreateBlendState failed");
        return false;
    }
    return true;
}

// ─── 私有：动态顶点缓冲区 ────────────────────────────────────────────────────

bool GpuCompositor::create_vertex_buffer()
{
    auto* device = SharedD3D11Device::instance().device();

    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth      = sizeof(QuadVertex) * 4;
    vbd.Usage          = D3D11_USAGE_DYNAMIC;
    vbd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = device->CreateBuffer(&vbd, nullptr, vb_.put());
    if (FAILED(hr)) {
        LOG_ERROR("[GpuCompositor] CreateBuffer (VB) failed");
        return false;
    }
    return true;
}

// ─── 私有：绘制一个图层四边形 ────────────────────────────────────────────────

void GpuCompositor::draw_quad(ID3D11ShaderResourceView* srv,
                               float x, float y, float w, float h,
                               float opacity)
{
    if (!srv) return;
    auto* ctx = SharedD3D11Device::instance().context();

    // 画布坐标 → NDC（Y 轴翻转：D3D11 NDC Y 向上，画布 Y 向下）
    const float cw = static_cast<float>(canvas_width_);
    const float ch = static_cast<float>(canvas_height_);
    const float l  = (x        / cw) * 2.f - 1.f;
    const float r  = ((x + w)  / cw) * 2.f - 1.f;
    const float t  = 1.f - (y        / ch) * 2.f;
    const float b  = 1.f - ((y + h)  / ch) * 2.f;

    // Triangle Strip: TL → TR → BL → BR
    QuadVertex verts[4] = {
        { l, t, 0.f, 0.f },   // top-left
        { r, t, 1.f, 0.f },   // top-right
        { l, b, 0.f, 1.f },   // bottom-left
        { r, b, 1.f, 1.f },   // bottom-right
    };

    // 更新顶点缓冲区
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(vb_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    memcpy(mapped.pData, verts, sizeof(verts));
    ctx->Unmap(vb_.get(), 0);

    // 更新常量缓冲区（opacity）
    CBPerLayer cb{ opacity, { 0.f, 0.f, 0.f } };
    if (FAILED(ctx->Map(cb_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    memcpy(mapped.pData, &cb, sizeof(cb));
    ctx->Unmap(cb_.get(), 0);

    // 绑定资源并绘制
    UINT stride = sizeof(QuadVertex), offset = 0;
    ctx->IASetVertexBuffers(0, 1, vb_.put(), &stride, &offset);
    ID3D11Buffer* cb_ptr = cb_.get();
    ctx->PSSetConstantBuffers(0, 1, &cb_ptr);
    ctx->PSSetShaderResources(0, 1, &srv);

    ctx->Draw(4, 0);

    // 解绑 SRV 避免资源冲突
    ID3D11ShaderResourceView* null_srv = nullptr;
    ctx->PSSetShaderResources(0, 1, &null_srv);
}

} // namespace live_assistant
