#include "scene_manager/gpu_color_converter.h"
#include "scene_manager/shared_d3d_device.h"
#include "common/log.h"

#include <string>

namespace live_assistant {

GpuColorConverter::GpuColorConverter() = default;
GpuColorConverter::~GpuColorConverter() { shutdown(); }

bool GpuColorConverter::initialize(int input_w, int input_h,
                                    int output_w, int output_h,
                                    DXGI_FORMAT input_format)
{
    if (initialized_) shutdown();

    auto& shared = SharedD3D11Device::instance();
    video_device_  = shared.video_device();   // 来自 Phase 0
    video_context_ = shared.video_context();  // 来自 Phase 0

    if (!video_device_ || !video_context_) {
        LOG_ERROR("[GpuColorConverter] ID3D11VideoDevice/Context not available "
                  "(GPU may not support D3D11 video processing)");
        return false;
    }

    input_width_   = input_w;
    input_height_  = input_h;
    output_width_  = output_w;
    output_height_ = output_h;
    input_format_  = input_format;

    // ── 1. 创建 Video Processor Enumerator ──────────────────────────────────
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content_desc{};
    content_desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content_desc.InputWidth       = static_cast<UINT>(input_w);
    content_desc.InputHeight      = static_cast<UINT>(input_h);
    content_desc.OutputWidth      = static_cast<UINT>(output_w);
    content_desc.OutputHeight     = static_cast<UINT>(output_h);
    content_desc.Usage            = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    HRESULT hr = video_device_->CreateVideoProcessorEnumerator(
        &content_desc, enumerator_.put());
    if (FAILED(hr)) {
        LOG_ERROR("[GpuColorConverter] CreateVideoProcessorEnumerator failed: hr=" +
                  std::to_string(hr));
        return false;
    }

    // ── 2. 检查 NV12 输出格式支持 ────────────────────────────────────────────
    UINT support_flags = 0;
    hr = enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support_flags);
    if (FAILED(hr) || !(support_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        LOG_WARNING("[GpuColorConverter] NV12 output not supported by VideoProcessor, "
                    "will need fallback");
        // 不立即返回失败——后续 convert 失败时上层会回退 sws_scale
    }

    // ── 3. 创建 Video Processor ──────────────────────────────────────────────
    hr = video_device_->CreateVideoProcessor(enumerator_.get(), 0, processor_.put());
    if (FAILED(hr)) {
        LOG_ERROR("[GpuColorConverter] CreateVideoProcessor failed: hr=" +
                  std::to_string(hr));
        return false;
    }

    // ── 4. 设置色彩空间（BT.709，有限范围，适合 HD 内容）──────────────────
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs{};
    cs.Usage         = 0;   // 播放（非处理）
    cs.RGB_Range     = 1;   // 有限范围（16-235）
    cs.YCbCr_Matrix  = 1;   // BT.709
    cs.YCbCr_xvYCC   = 0;
    cs.Nominal_Range = 2;   // 16-235（D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235）
    video_context_->VideoProcessorSetStreamColorSpace(processor_.get(), 0, &cs);
    video_context_->VideoProcessorSetOutputColorSpace(processor_.get(), &cs);

    // ── 5. 创建 NV12 输出纹理 + 输出视图 ─────────────────────────────────────
    if (!create_nv12_texture(output_w, output_h)) return false;
    if (!create_output_view()) return false;

    initialized_ = true;
    LOG_INFO("[GpuColorConverter] initialized: " +
             std::to_string(input_w) + "x" + std::to_string(input_h) +
             " BGRA → " +
             std::to_string(output_w) + "x" + std::to_string(output_h) + " NV12");
    return true;
}

GpuTextureRef GpuColorConverter::convert(ID3D11Texture2D* input_bgra)
{
    if (!initialized_ || !input_bgra) return {};

    // ── 创建输入视图（InputView 按纹理实例创建，不缓存）────────────────────
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv_desc{};
    iv_desc.FourCC          = 0;
    iv_desc.ViewDimension   = D3D11_VPIV_DIMENSION_TEXTURE2D;
    iv_desc.Texture2D.MipSlice = 0;

    winrt::com_ptr<ID3D11VideoProcessorInputView> input_view;
    HRESULT hr = video_device_->CreateVideoProcessorInputView(
        input_bgra, enumerator_.get(), &iv_desc, input_view.put());
    if (FAILED(hr)) {
        LOG_ERROR("[GpuColorConverter] CreateVideoProcessorInputView failed: hr=" +
                  std::to_string(hr) +
                  " (纹理可能缺少 D3D11_BIND_DECODER 标志，请检查输入纹理 BindFlags)");
        return {};
    }

    // ── 执行转换 ─────────────────────────────────────────────────────────────
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable         = TRUE;
    stream.pInputSurface  = input_view.get();

    hr = video_context_->VideoProcessorBlt(
        processor_.get(), output_view_.get(), 0, 1, &stream);
    if (FAILED(hr)) {
        LOG_ERROR("[GpuColorConverter] VideoProcessorBlt failed: hr=" + std::to_string(hr));
        return {};
    }

    GpuTextureRef result;
    result.texture = nv12_texture_;
    result.width   = static_cast<uint32_t>(output_width_);
    result.height  = static_cast<uint32_t>(output_height_);
    result.format  = DXGI_FORMAT_NV12;
    return result;
}

void GpuColorConverter::shutdown()
{
    if (!initialized_) return;
    output_view_   = nullptr;
    nv12_texture_  = nullptr;
    processor_     = nullptr;
    enumerator_    = nullptr;
    // 裸指针，不 Release（由 SharedD3D11Device 管理）
    video_context_ = nullptr;
    video_device_  = nullptr;
    initialized_   = false;
    LOG_INFO("[GpuColorConverter] shutdown");
}

// ─── 私有 ────────────────────────────────────────────────────────────────────

bool GpuColorConverter::create_nv12_texture(int w, int h)
{
    auto& shared = SharedD3D11Device::instance();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width          = static_cast<UINT>(w);
    desc.Height         = static_cast<UINT>(h);
    desc.MipLevels      = 1;
    desc.ArraySize      = 1;
    desc.Format         = DXGI_FORMAT_NV12;
    desc.SampleDesc     = { 1, 0 };
    desc.Usage          = D3D11_USAGE_DEFAULT;
    // Intel iGPU 驱动兼容性说明：
    // - BIND_RENDER_TARGET | BIND_DECODER：此 Intel 驱动 CreateTexture2D 返回 E_INVALIDARG。
    // - BIND_DECODER 单独使用：CreateTexture2D 成功，但 CreateVideoProcessorOutputView 返回
    //   E_INVALIDARG（此 Intel 驱动要求输出纹理必须有 BIND_RENDER_TARGET）。
    // - BIND_RENDER_TARGET 单独使用：CreateTexture2D 和 CreateVideoProcessorOutputView 均成功。
    //   VideoProcessorBlt 写入后纹理可能产生 RC/CCS（Render Compressed）格式。
    //   但 compositor_encoder_bridge 已改用 CopySubresourceRegion 按 NV12 平面分别拷贝，
    //   可绕过 RC/CCS 导致的 CopyResource 崩溃。因此 BIND_RENDER_TARGET 是目前唯一可行选项。
    desc.BindFlags      = D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = 0;

    HRESULT hr = shared.device()->CreateTexture2D(&desc, nullptr, nv12_texture_.put());
    if (FAILED(hr)) {
        LOG_ERROR("[GpuColorConverter] CreateTexture2D NV12 failed: hr=" + std::to_string(hr));
        return false;
    }
    return true;
}

bool GpuColorConverter::create_output_view()
{
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov_desc{};
    ov_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ov_desc.Texture2D.MipSlice = 0;

    HRESULT hr = video_device_->CreateVideoProcessorOutputView(
        nv12_texture_.get(), enumerator_.get(), &ov_desc, output_view_.put());
    if (FAILED(hr)) {
        LOG_ERROR("[GpuColorConverter] CreateVideoProcessorOutputView failed: hr=" +
                  std::to_string(hr));
        return false;
    }
    return true;
}

} // namespace live_assistant
