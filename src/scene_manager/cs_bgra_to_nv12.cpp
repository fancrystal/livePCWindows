#include "scene_manager/cs_bgra_to_nv12.h"
#include "scene_manager/shared_d3d_device.h"
#include "video_engine/video_engine.h"   // VideoFrame
#include "common/log.h"

#include <d3dcompiler.h>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

namespace live_assistant {

// ---------------------------------------------------------------------------
// BGRA → NV12 Compute Shader（BT.601 limited range）
//
//   输入: BGRA SRV（B8G8R8A8_UNORM）。注意：D3D 中 B8G8R8A8 通过 Texture2D<float4>
//         读取后，float4.x=B, .y=G, .z=R, .w=A。
//   输出: R8 (Y)、R8G8 (UV)。
//
// BT.601 limited range：
//   Y'  =   0.257 R + 0.504 G + 0.098 B + 16/255
//   Cb  =  -0.148 R - 0.291 G + 0.439 B + 128/255
//   Cr  =   0.439 R - 0.368 G - 0.071 B + 128/255
//
// 注意：UV 采样位置为 2x2 块的左上像素（FFmpeg 默认行为，与 sws_scale 一致）。
// ---------------------------------------------------------------------------
static const char* kBgraToNv12Shader = R"(
Texture2D<float4>      bgra_in : register(t0);
RWTexture2D<float>     nv12_y  : register(u0);
RWTexture2D<float2>    nv12_uv : register(u1);

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    float4 c = bgra_in.Load(int3(id.xy, 0));
    float b = c.x;
    float g = c.y;
    float r = c.z;

    // Y' (limited range)
    float y = 0.257 * r + 0.504 * g + 0.098 * b + (16.0 / 255.0);
    nv12_y[id.xy] = saturate(y);

    // UV: 2x2 块的左上像素采样
    if ((id.x & 1u) == 0u && (id.y & 1u) == 0u)
    {
        float cb = -0.148 * r - 0.291 * g + 0.439 * b + (128.0 / 255.0);
        float cr =  0.439 * r - 0.368 * g - 0.071 * b + (128.0 / 255.0);
        nv12_uv[uint2(id.x / 2u, id.y / 2u)] = float2(saturate(cb), saturate(cr));
    }
}
)";

CsBgraToNv12::CsBgraToNv12() = default;

CsBgraToNv12::~CsBgraToNv12()
{
    shutdown();
}

bool CsBgraToNv12::initialize(int width, int height)
{
    if (initialized_) shutdown();

    if (width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
        LOG_ERROR("[CsBgraToNv12] invalid size " + std::to_string(width) +
                  "x" + std::to_string(height) + " (must be positive and even)");
        return false;
    }

    width_  = width;
    height_ = height;

    if (!create_compute_shader()) {
        return false;
    }
    if (!create_textures(width, height)) {
        return false;
    }

    initialized_ = true;
    LOG_INFO("[CsBgraToNv12] initialized: " +
             std::to_string(width) + "x" + std::to_string(height));
    return true;
}

void CsBgraToNv12::shutdown()
{
    cs_.Reset();
    uav_y_.Reset();
    uav_uv_.Reset();
    uav_y_texture_.Reset();
    uav_uv_texture_.Reset();
    staging_y_.Reset();
    staging_uv_.Reset();
    initialized_ = false;
    width_  = 0;
    height_ = 0;
}

bool CsBgraToNv12::create_compute_shader()
{
    auto& shared = SharedD3D11Device::instance();
    ID3D11Device* device = shared.device();
    if (!device) {
        LOG_ERROR("[CsBgraToNv12] D3D11 device not available");
        return false;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> cs_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> err_blob;

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG;
#endif

    HRESULT hr = D3DCompile(
        kBgraToNv12Shader, strlen(kBgraToNv12Shader),
        nullptr, nullptr, nullptr,
        "main", "cs_5_0",
        flags, 0,
        cs_blob.GetAddressOf(),
        err_blob.GetAddressOf());

    if (FAILED(hr)) {
        std::string msg = "[CsBgraToNv12] D3DCompile failed hr=0x" +
                          std::to_string(static_cast<uint32_t>(hr));
        if (err_blob) {
            msg += " err=";
            msg.append(static_cast<const char*>(err_blob->GetBufferPointer()),
                       err_blob->GetBufferSize());
        }
        LOG_ERROR(msg);
        return false;
    }

    hr = device->CreateComputeShader(
        cs_blob->GetBufferPointer(),
        cs_blob->GetBufferSize(),
        nullptr,
        cs_.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("[CsBgraToNv12] CreateComputeShader failed hr=0x" +
                  std::to_string(static_cast<uint32_t>(hr)));
        return false;
    }
    return true;
}

bool CsBgraToNv12::create_textures(int width, int height)
{
    auto& shared = SharedD3D11Device::instance();
    ID3D11Device* device = shared.device();
    if (!device) return false;

    // ── UAV Y 纹理 (W x H, R8_UNORM) ────────────────────────────
    D3D11_TEXTURE2D_DESC desc_y{};
    desc_y.Width            = static_cast<UINT>(width);
    desc_y.Height           = static_cast<UINT>(height);
    desc_y.MipLevels        = 1;
    desc_y.ArraySize        = 1;
    desc_y.Format           = DXGI_FORMAT_R8_UNORM;
    desc_y.SampleDesc.Count = 1;
    desc_y.Usage            = D3D11_USAGE_DEFAULT;
    desc_y.BindFlags        = D3D11_BIND_UNORDERED_ACCESS;

    HRESULT hr = device->CreateTexture2D(&desc_y, nullptr, uav_y_texture_.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("[CsBgraToNv12] CreateTexture2D Y UAV failed hr=0x" +
                  std::to_string(static_cast<uint32_t>(hr)));
        return false;
    }

    // ── UAV UV 纹理 (W/2 x H/2, R8G8_UNORM) ─────────────────────
    D3D11_TEXTURE2D_DESC desc_uv{};
    desc_uv.Width            = static_cast<UINT>(width  / 2);
    desc_uv.Height           = static_cast<UINT>(height / 2);
    desc_uv.MipLevels        = 1;
    desc_uv.ArraySize        = 1;
    desc_uv.Format           = DXGI_FORMAT_R8G8_UNORM;
    desc_uv.SampleDesc.Count = 1;
    desc_uv.Usage            = D3D11_USAGE_DEFAULT;
    desc_uv.BindFlags        = D3D11_BIND_UNORDERED_ACCESS;

    hr = device->CreateTexture2D(&desc_uv, nullptr, uav_uv_texture_.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("[CsBgraToNv12] CreateTexture2D UV UAV failed hr=0x" +
                  std::to_string(static_cast<uint32_t>(hr)));
        return false;
    }

    // ── Y UAV ────────────────────────────────────────────────────
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc_y{};
    uav_desc_y.Format             = DXGI_FORMAT_R8_UNORM;
    uav_desc_y.ViewDimension      = D3D11_UAV_DIMENSION_TEXTURE2D;
    uav_desc_y.Texture2D.MipSlice = 0;

    hr = device->CreateUnorderedAccessView(
        uav_y_texture_.Get(), &uav_desc_y, uav_y_.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("[CsBgraToNv12] CreateUAV Y failed hr=0x" +
                  std::to_string(static_cast<uint32_t>(hr)));
        return false;
    }

    // ── UV UAV ───────────────────────────────────────────────────
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc_uv{};
    uav_desc_uv.Format             = DXGI_FORMAT_R8G8_UNORM;
    uav_desc_uv.ViewDimension      = D3D11_UAV_DIMENSION_TEXTURE2D;
    uav_desc_uv.Texture2D.MipSlice = 0;

    hr = device->CreateUnorderedAccessView(
        uav_uv_texture_.Get(), &uav_desc_uv, uav_uv_.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("[CsBgraToNv12] CreateUAV UV failed hr=0x" +
                  std::to_string(static_cast<uint32_t>(hr)));
        return false;
    }

    // ── STAGING Y / UV (GPU → CPU 回读) ─────────────────────────
    D3D11_TEXTURE2D_DESC sd_y = desc_y;
    sd_y.BindFlags            = 0;
    sd_y.Usage                = D3D11_USAGE_STAGING;
    sd_y.CPUAccessFlags       = D3D11_CPU_ACCESS_READ;
    hr = device->CreateTexture2D(&sd_y, nullptr, staging_y_.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("[CsBgraToNv12] CreateTexture2D Y staging failed hr=0x" +
                  std::to_string(static_cast<uint32_t>(hr)));
        return false;
    }

    D3D11_TEXTURE2D_DESC sd_uv = desc_uv;
    sd_uv.BindFlags            = 0;
    sd_uv.Usage                = D3D11_USAGE_STAGING;
    sd_uv.CPUAccessFlags       = D3D11_CPU_ACCESS_READ;
    hr = device->CreateTexture2D(&sd_uv, nullptr, staging_uv_.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("[CsBgraToNv12] CreateTexture2D UV staging failed hr=0x" +
                  std::to_string(static_cast<uint32_t>(hr)));
        return false;
    }

    return true;
}

bool CsBgraToNv12::dispatch_only(ID3D11ShaderResourceView* bgra_srv)
{
    if (!initialized_ || !bgra_srv) {
        LOG_ERROR("[CsBgraToNv12] dispatch_only: not initialized or null SRV");
        return false;
    }

    auto& shared = SharedD3D11Device::instance();
    ID3D11DeviceContext* ctx = shared.context();
    if (!ctx) {
        LOG_ERROR("[CsBgraToNv12] dispatch_only: D3D11 context unavailable");
        return false;
    }

    ID3D11ShaderResourceView*  srvs[] = { bgra_srv };
    ID3D11UnorderedAccessView* uavs[] = { uav_y_.Get(), uav_uv_.Get() };

    ctx->CSSetShader(cs_.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 1, srvs);
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

    // 16x16 线程组：每个线程处理一个 BGRA 像素，每 2x2 写一个 UV
    UINT tgx = static_cast<UINT>((width_  + 15) / 16);
    UINT tgy = static_cast<UINT>((height_ + 15) / 16);
    ctx->Dispatch(tgx, tgy, 1);

    // 清理绑定避免后续 RTV/UAV 冲突
    ID3D11ShaderResourceView*  null_srvs[1] = { nullptr };
    ID3D11UnorderedAccessView* null_uavs[2] = { nullptr, nullptr };
    ctx->CSSetShaderResources(0, 1, null_srvs);
    ctx->CSSetUnorderedAccessViews(0, 2, null_uavs, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);

    return true;
}

std::shared_ptr<VideoFrame> CsBgraToNv12::convert_to_cpu(
    ID3D11ShaderResourceView* bgra_srv,
    int64_t timestamp_ms)
{
    if (!initialized_ || !bgra_srv) {
        LOG_ERROR("[CsBgraToNv12] convert_to_cpu: not initialized or null SRV");
        return nullptr;
    }

    auto& shared = SharedD3D11Device::instance();
    ID3D11DeviceContext* ctx = shared.context();
    if (!ctx) {
        LOG_ERROR("[CsBgraToNv12] convert_to_cpu: D3D11 context unavailable");
        return nullptr;
    }

    // ── 1. CS Dispatch: BGRA → R8 (Y) + R8G8 (UV) ───────────────
    if (!dispatch_only(bgra_srv)) {
        return nullptr;
    }

    // ── 2. UAV → STAGING（同格式，无跨格式拷贝问题）─────────────
    ctx->CopyResource(staging_y_.Get(),  uav_y_texture_.Get());
    ctx->CopyResource(staging_uv_.Get(), uav_uv_texture_.Get());

    // ── 3. Map STAGING，回读到 CPU ──────────────────────────────
    D3D11_MAPPED_SUBRESOURCE mapped_y{};
    HRESULT hr = ctx->Map(staging_y_.Get(), 0, D3D11_MAP_READ, 0, &mapped_y);
    if (FAILED(hr)) {
        LOG_ERROR("[CsBgraToNv12] Map Y staging failed hr=0x" +
                  std::to_string(static_cast<uint32_t>(hr)));
        return nullptr;
    }

    D3D11_MAPPED_SUBRESOURCE mapped_uv{};
    hr = ctx->Map(staging_uv_.Get(), 0, D3D11_MAP_READ, 0, &mapped_uv);
    if (FAILED(hr)) {
        ctx->Unmap(staging_y_.Get(), 0);
        LOG_ERROR("[CsBgraToNv12] Map UV staging failed hr=0x" +
                  std::to_string(static_cast<uint32_t>(hr)));
        return nullptr;
    }

    // ── 4. 构造 NV12 VideoFrame ─────────────────────────────────
    auto frame          = std::make_shared<VideoFrame>();
    frame->format       = VideoFrame::PixelFormat::NV12;
    frame->width        = width_;
    frame->height       = height_;
    frame->stride       = width_;  // NV12 Y: 1 byte / pixel
    frame->stride_uv    = width_;  // NV12 UV: width/2 对 × 2 字节 = width 字节
    frame->timestamp_ms = timestamp_ms;

    const size_t y_size  = static_cast<size_t>(width_) * height_;
    const size_t uv_size = static_cast<size_t>(width_) * (height_ / 2);
    frame->data    = std::make_unique<uint8_t[]>(y_size);
    frame->data_uv = std::make_unique<uint8_t[]>(uv_size);

    // 逐行 memcpy：STAGING 的 RowPitch 通常 >= width，含对齐填充
    const int copy_w_y  = width_;
    for (int y = 0; y < height_; ++y) {
        memcpy(frame->data.get() + static_cast<size_t>(y) * copy_w_y,
               static_cast<const uint8_t*>(mapped_y.pData) +
                   static_cast<size_t>(y) * mapped_y.RowPitch,
               copy_w_y);
    }
    const int uv_h     = height_ / 2;
    const int copy_w_uv = width_;  // R8G8 width/2 像素 × 2 字节
    for (int y = 0; y < uv_h; ++y) {
        memcpy(frame->data_uv.get() + static_cast<size_t>(y) * copy_w_uv,
               static_cast<const uint8_t*>(mapped_uv.pData) +
                   static_cast<size_t>(y) * mapped_uv.RowPitch,
               copy_w_uv);
    }

    ctx->Unmap(staging_y_.Get(),  0);
    ctx->Unmap(staging_uv_.Get(), 0);

    return frame;
}

} // namespace live_assistant
