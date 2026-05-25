#include "scene_manager/gpu_nv12_copier.h"
#include "scene_manager/shared_d3d_device.h"
#include "video_engine/video_engine.h"   // VideoFrame
#include "common/log.h"
#include <d3dcompiler.h>
#include <algorithm>

namespace live_assistant {

// NV12 Copy Compute Shader (HLSL)
// 从 NV12 SRV 读取 Y/UV 平面，写入 UAV
static const char* kNv12CopyShader = R"(
Texture2D<float> y_plane : register(t0);
Texture2D<float2> uv_plane : register(t1);

RWTexture2D<float> out_y : register(u0);
RWTexture2D<float2> out_uv : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // Y plane (full resolution)
    out_y[id.xy] = y_plane[id.xy];

    // UV plane (quarter resolution)
    if (id.x % 2 == 0 && id.y % 2 == 0)
    {
        uint2 uv_id = id.xy / 2;
        out_uv[uv_id] = uv_plane[uv_id];
    }
}
)";

GpuNv12Copier::GpuNv12Copier() = default;

GpuNv12Copier::~GpuNv12Copier() {
    shutdown();
}

bool GpuNv12Copier::initialize(int width, int height) {
    if (initialized_) shutdown();

    width_ = width;
    height_ = height;

    if (!create_compute_shader()) {
        LOG_ERROR("[GpuNv12Copier] Failed to create compute shader");
        return false;
    }

    if (!create_uav_texture(width, height)) {
        LOG_ERROR("[GpuNv12Copier] Failed to create UAV/staging textures");
        return false;
    }

    initialized_ = true;
    LOG_INFO("[GpuNv12Copier] Initialized: " + std::to_string(width) + "x" + std::to_string(height));
    return true;
}

void GpuNv12Copier::shutdown() {
    cs_.Reset();
    uav_y_texture_.Reset();
    uav_uv_texture_.Reset();
    uav_y_.Reset();
    uav_uv_.Reset();
    staging_y_.Reset();
    staging_uv_.Reset();
    src_srv_y_.Reset();
    src_srv_uv_.Reset();
    initialized_ = false;
}

bool GpuNv12Copier::create_compute_shader() {
    auto& shared = SharedD3D11Device::instance();
    ID3D11Device* device = shared.device();
    if (!device) {
        LOG_ERROR("[GpuNv12Copier] D3D11 device not available");
        return false;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> cs_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> error_blob;

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG;
#endif

    HRESULT hr = D3DCompile(kNv12CopyShader, strlen(kNv12CopyShader),
                           nullptr, nullptr, nullptr,
                           "main", "cs_5_0",
                           flags, 0,
                           cs_blob.GetAddressOf(),
                           error_blob.GetAddressOf());

    if (FAILED(hr)) {
        if (error_blob) {
            LOG_ERROR("[GpuNv12Copier] CS compile failed: " +
                     std::string(static_cast<char*>(error_blob->GetBufferPointer())));
        }
        return false;
    }

    hr = device->CreateComputeShader(cs_blob->GetBufferPointer(),
                                    cs_blob->GetBufferSize(),
                                    nullptr, cs_.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("[GpuNv12Copier] CreateComputeShader failed: hr=" + std::to_string(hr));
        return false;
    }

    return true;
}

bool GpuNv12Copier::create_uav_texture(int width, int height) {
    auto& shared = SharedD3D11Device::instance();
    ID3D11Device* device = shared.device();
    if (!device) return false;

    // ── Y 平面 UAV 纹理（W×H, R8_UNORM）────────────────────────────
    D3D11_TEXTURE2D_DESC desc_y = {};
    desc_y.Width  = static_cast<UINT>(width);
    desc_y.Height = static_cast<UINT>(height);
    desc_y.MipLevels = 1;
    desc_y.ArraySize = 1;
    desc_y.Format = DXGI_FORMAT_R8_UNORM;
    desc_y.SampleDesc.Count = 1;
    desc_y.Usage = D3D11_USAGE_DEFAULT;
    desc_y.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

    HRESULT hr = device->CreateTexture2D(&desc_y, nullptr, uav_y_texture_.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("[GpuNv12Copier] CreateTexture2D Y UAV failed: hr=0x" +
                  std::to_string(static_cast<unsigned long>(hr)));
        return false;
    }

    // ── UV 平面 UAV 纹理（W/2×H/2, R8G8_UNORM）─────────────────────
    D3D11_TEXTURE2D_DESC desc_uv = {};
    desc_uv.Width  = static_cast<UINT>(width  / 2);
    desc_uv.Height = static_cast<UINT>(height / 2);
    desc_uv.MipLevels = 1;
    desc_uv.ArraySize = 1;
    desc_uv.Format = DXGI_FORMAT_R8G8_UNORM;
    desc_uv.SampleDesc.Count = 1;
    desc_uv.Usage = D3D11_USAGE_DEFAULT;
    desc_uv.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

    hr = device->CreateTexture2D(&desc_uv, nullptr, uav_uv_texture_.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("[GpuNv12Copier] CreateTexture2D UV UAV failed: hr=0x" +
                  std::to_string(static_cast<unsigned long>(hr)));
        return false;
    }

    // ── Y 平面 STAGING 纹理（GPU→CPU 回读）──────────────────────────
    D3D11_TEXTURE2D_DESC staging_desc_y = desc_y;
    staging_desc_y.BindFlags = 0;
    staging_desc_y.Usage = D3D11_USAGE_STAGING;
    staging_desc_y.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = device->CreateTexture2D(&staging_desc_y, nullptr, staging_y_.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("[GpuNv12Copier] CreateTexture2D Y staging failed: hr=0x" +
                  std::to_string(static_cast<unsigned long>(hr)));
        return false;
    }

    // ── UV 平面 STAGING 纹理（GPU→CPU 回读）─────────────────────────
    D3D11_TEXTURE2D_DESC staging_desc_uv = desc_uv;
    staging_desc_uv.BindFlags = 0;
    staging_desc_uv.Usage = D3D11_USAGE_STAGING;
    staging_desc_uv.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = device->CreateTexture2D(&staging_desc_uv, nullptr, staging_uv_.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("[GpuNv12Copier] CreateTexture2D UV staging failed: hr=0x" +
                  std::to_string(static_cast<unsigned long>(hr)));
        return false;
    }

    // ── Y UAV ────────────────────────────────────────────────────────
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc_y = {};
    uav_desc_y.Format = DXGI_FORMAT_R8_UNORM;
    uav_desc_y.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    uav_desc_y.Texture2D.MipSlice = 0;

    hr = device->CreateUnorderedAccessView(uav_y_texture_.Get(), &uav_desc_y, uav_y_.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("[GpuNv12Copier] CreateUAV Y failed: hr=0x" +
                  std::to_string(static_cast<unsigned long>(hr)));
        return false;
    }

    // ── UV UAV ───────────────────────────────────────────────────────
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc_uv = {};
    uav_desc_uv.Format = DXGI_FORMAT_R8G8_UNORM;
    uav_desc_uv.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    uav_desc_uv.Texture2D.MipSlice = 0;

    hr = device->CreateUnorderedAccessView(uav_uv_texture_.Get(), &uav_desc_uv, uav_uv_.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("[GpuNv12Copier] CreateUAV UV failed: hr=0x" +
                  std::to_string(static_cast<unsigned long>(hr)));
        return false;
    }

    return true;
}

std::shared_ptr<VideoFrame> GpuNv12Copier::readback_nv12(ID3D11Texture2D* src_texture) {
    if (!initialized_ || !src_texture) {
        LOG_ERROR("[GpuNv12Copier] readback_nv12: invalid params or not initialized");
        return nullptr;
    }

    try {
        auto& shared = SharedD3D11Device::instance();
        ID3D11Device* device = shared.device();
        ID3D11DeviceContext* ctx = shared.context();
        if (!device || !ctx) {
            LOG_ERROR("[GpuNv12Copier] readback_nv12: device or context unavailable");
            return nullptr;
        }

        // ── 1. 惰性创建 D3D11.3 PlaneSlice SRVs ────────────────────────
        if (!src_srv_y_ || !src_srv_uv_) {
            Microsoft::WRL::ComPtr<ID3D11Device3> device3;
            HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&device3));
            if (FAILED(hr) || !device3) {
                LOG_ERROR("[GpuNv12Copier] Failed to get ID3D11Device3: hr=0x" +
                          std::to_string(static_cast<unsigned long>(hr)));
                return nullptr;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC1 srv_desc_y = {};
            srv_desc_y.Format = DXGI_FORMAT_R8_UNORM;
            srv_desc_y.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv_desc_y.Texture2D.MostDetailedMip = 0;
            srv_desc_y.Texture2D.MipLevels = 1;
            srv_desc_y.Texture2D.PlaneSlice = 0;

            hr = device3->CreateShaderResourceView1(src_texture, &srv_desc_y, src_srv_y_.GetAddressOf());
            if (FAILED(hr)) {
                LOG_ERROR("[GpuNv12Copier] CreateSRV1 Y failed: hr=0x" +
                          std::to_string(static_cast<unsigned long>(hr)));
                return nullptr;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC1 srv_desc_uv = {};
            srv_desc_uv.Format = DXGI_FORMAT_R8G8_UNORM;
            srv_desc_uv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv_desc_uv.Texture2D.MostDetailedMip = 0;
            srv_desc_uv.Texture2D.MipLevels = 1;
            srv_desc_uv.Texture2D.PlaneSlice = 1;

            hr = device3->CreateShaderResourceView1(src_texture, &srv_desc_uv, src_srv_uv_.GetAddressOf());
            if (FAILED(hr)) {
                LOG_ERROR("[GpuNv12Copier] CreateSRV1 UV failed: hr=0x" +
                          std::to_string(static_cast<unsigned long>(hr)));
                src_srv_y_.Reset();
                return nullptr;
            }

            LOG_DEBUG("[GpuNv12Copier] Created D3D11.3 PlaneSlice SRVs for NV12 source texture");
        }

        // ── 2. CS Dispatch: RC NV12 → R8/R8G8 UAV 纹理 ─────────────────
        ID3D11ShaderResourceView* srvs[] = { src_srv_y_.Get(), src_srv_uv_.Get() };
        ID3D11UnorderedAccessView* uavs[] = { uav_y_.Get(), uav_uv_.Get() };

        ctx->CSSetShader(cs_.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 2, srvs);
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

        UINT tgx = static_cast<UINT>((width_ + 7) / 8);
        UINT tgy = static_cast<UINT>((height_ + 7) / 8);
        ctx->Dispatch(tgx, tgy, 1);

        // Clear bindings
        ID3D11ShaderResourceView* null_srvs[] = { nullptr, nullptr };
        ID3D11UnorderedAccessView* null_uavs[] = { nullptr, nullptr };
        ctx->CSSetShaderResources(0, 2, null_srvs);
        ctx->CSSetUnorderedAccessViews(0, 2, null_uavs, nullptr);
        ctx->CSSetShader(nullptr, nullptr, 0);

        // ── 3. GPU→CPU: UAV 纹理 → STAGING 纹理（同格式，无跨格式问题）──
        ctx->CopyResource(staging_y_.Get(), uav_y_texture_.Get());
        ctx->CopyResource(staging_uv_.Get(), uav_uv_texture_.Get());

        // ── 4. Map STAGING 纹理，回读到 CPU ─────────────────────────────
        D3D11_MAPPED_SUBRESOURCE mapped_y{};
        HRESULT hr = ctx->Map(staging_y_.Get(), 0, D3D11_MAP_READ, 0, &mapped_y);
        if (FAILED(hr)) {
            LOG_ERROR("[GpuNv12Copier] Map Y staging failed: hr=0x" +
                      std::to_string(static_cast<unsigned long>(hr)));
            return nullptr;
        }

        D3D11_MAPPED_SUBRESOURCE mapped_uv{};
        hr = ctx->Map(staging_uv_.Get(), 0, D3D11_MAP_READ, 0, &mapped_uv);
        if (FAILED(hr)) {
            ctx->Unmap(staging_y_.Get(), 0);
            LOG_ERROR("[GpuNv12Copier] Map UV staging failed: hr=0x" +
                      std::to_string(static_cast<unsigned long>(hr)));
            return nullptr;
        }

        // ── 5. 创建 NV12 VideoFrame ─────────────────────────────────────
        auto frame = std::make_shared<VideoFrame>();
        frame->format = VideoFrame::PixelFormat::NV12;
        frame->width = width_;
        frame->height = height_;
        frame->stride = width_;           // NV12 Y: 1 byte per pixel
        frame->stride_uv = width_;        // NV12 UV: 2 bytes per pixel pair, width/2 pairs = width bytes

        size_t y_size = static_cast<size_t>(width_) * height_;
        size_t uv_size = static_cast<size_t>(width_) * (height_ / 2);
        frame->data = std::make_unique<uint8_t[]>(y_size);
        frame->data_uv = std::make_unique<uint8_t[]>(uv_size);

        // 逐行复制（STAGING 的 RowPitch 可能比 width 大，有对齐填充）
        const int copy_w = width_;
        for (int y = 0; y < height_; ++y) {
            memcpy(frame->data.get() + y * copy_w,
                   static_cast<const uint8_t*>(mapped_y.pData) + y * mapped_y.RowPitch,
                   copy_w);
        }
        const int uv_h = height_ / 2;
        const int uv_w = width_;  // R8G8_UNORM: width/2 pixels × 2 bytes = width bytes
        for (int y = 0; y < uv_h; ++y) {
            memcpy(frame->data_uv.get() + y * uv_w,
                   static_cast<const uint8_t*>(mapped_uv.pData) + y * mapped_uv.RowPitch,
                   uv_w);
        }

        ctx->Unmap(staging_y_.Get(), 0);
        ctx->Unmap(staging_uv_.Get(), 0);

        LOG_DEBUG("[GpuNv12Copier] readback_nv12 completed: " +
                  std::to_string(width_) + "x" + std::to_string(height_));
        return frame;

    } catch (const winrt::hresult_error& ex) {
        LOG_ERROR("[GpuNv12Copier] winrt::hresult_error in readback_nv12: " +
                  winrt::to_string(ex.message()));
        return nullptr;
    } catch (const std::exception& ex) {
        LOG_ERROR("[GpuNv12Copier] std::exception in readback_nv12: " + std::string(ex.what()));
        return nullptr;
    }
}

} // namespace live_assistant
