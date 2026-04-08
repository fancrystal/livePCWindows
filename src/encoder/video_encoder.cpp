#define NOMINMAX  // 防止 Windows 头文件定义 min/max 宏，与 std::min/max 冲突
#include "encoder/video_encoder.h"
#include "common/log.h"
#include "common/error.h"
#include "video_engine/video_engine.h"  // For VideoFrame definition
#include "scene_manager/shared_d3d_device.h"
#if defined(_MSC_VER)
#include <intrin.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include <d3d11.h>
#include <dxgi.h>

namespace live_assistant {

H264Encoder::H264Encoder() {
    LOG_INFO("H264Encoder constructor");
}

H264Encoder::~H264Encoder() {
    shutdown();
    LOG_INFO("H264Encoder destructor");
}

bool H264Encoder::initialize_shared_d3d11va()
{
    auto& shared = SharedD3D11Device::instance();
    if (!shared.device() || !shared.context()) {
        LOG_WARNING("[H264Encoder] SharedD3D11Device not ready, skipping D3D11VA sharing");
        return false;
    }

    d3d11va_device_ctx_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!d3d11va_device_ctx_) {
        LOG_ERROR("[H264Encoder] av_hwdevice_ctx_alloc(D3D11VA) failed");
        return false;
    }

    auto* hw_dev_ctx  = reinterpret_cast<AVHWDeviceContext*>(d3d11va_device_ctx_->data);
    auto* d3d11va_ctx = reinterpret_cast<AVD3D11VADeviceContext*>(hw_dev_ctx->hwctx);

    // Pre-fill with SharedD3D11Device's device/context.
    // FFmpeg will call Release() when the AVHWDeviceContext is freed, so AddRef() here.
    d3d11va_ctx->device         = shared.device();
    d3d11va_ctx->device_context = shared.context();
    d3d11va_ctx->device->AddRef();
    d3d11va_ctx->device_context->AddRef();
    // video_device / video_context: leave nullptr; FFmpeg will QueryInterface them
    d3d11va_ctx->video_device   = nullptr;
    d3d11va_ctx->video_context  = nullptr;
    // lock/unlock: nullptr — SharedD3D11Device already has ID3D11Multithread protection
    d3d11va_ctx->lock   = nullptr;
    d3d11va_ctx->unlock = nullptr;

    int ret = av_hwdevice_ctx_init(d3d11va_device_ctx_);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_WARNING("[H264Encoder] D3D11VA device init failed: " + std::string(errbuf) +
                    " — will use independent QSV device");
        av_buffer_unref(&d3d11va_device_ctx_);
        return false;
    }

    LOG_INFO("[H264Encoder] D3D11VA device context initialized from SharedD3D11Device (Phase 4)");
    return true;
}

int H264Encoder::detect_discrete_gpu() {
    // 返回：0=只有集显/软编路径, 1=NVIDIA独显, 2=AMD独显(不含集显), 3=NVIDIA+AMD都有
    int result = 0;

    winrt::com_ptr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                    reinterpret_cast<void**>(factory.put()));
    if (FAILED(hr)) {
        LOG_WARNING("[H264Encoder] detect_discrete_gpu: CreateDXGIFactory1 failed, hr=0x" +
                   std::to_string(static_cast<uint32_t>(hr)));
        return 0;
    }

    winrt::com_ptr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);

        std::wstring name(desc.Description);
        std::string name_utf8;
        {
            int len = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                name_utf8.resize(static_cast<size_t>(len - 1));
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, &name_utf8[0], len, nullptr, nullptr);
            }
        }

        bool is_nvidia = name.find(L"NVIDIA") != std::wstring::npos ||
                         name.find(L"GeForce") != std::wstring::npos;
        bool is_amd = name.find(L"AMD") != std::wstring::npos ||
                     name.find(L"Radeon") != std::wstring::npos;

        // 检查是否是独立显卡（非软件渲染，VRAM > 128MB 通常是独显）
        bool is_discrete = (desc.VendorId != 0x1414 &&  // 不是 Microsoft Software Renderer
                          (desc.DedicatedVideoMemory > 128 * 1024 * 1024 ||  // 有独立显存
                           desc.DedicatedSystemMemory > 64 * 1024 * 1024)); // 或独立系统内存

        if (is_nvidia && is_discrete) {
            result |= 1;  // bit 0 = NVIDIA 独显
            LOG_INFO("[H264Encoder] GPU detected: NVIDIA (dGPU) - " + name_utf8);
        } else if (is_amd && is_discrete) {
            result |= 2;  // bit 1 = AMD 独显
            LOG_INFO("[H264Encoder] GPU detected: AMD (dGPU) - " + name_utf8);
        } else if (!name_utf8.empty()) {
            LOG_INFO("[H264Encoder] GPU detected: " + name_utf8 +
                    (is_discrete ? " (iGPU)" : " (unknown)"));
        }

        adapter = nullptr;
    }

    LOG_INFO("[H264Encoder] GPU detection result: " + std::to_string(result) +
             " (0=none, 1=NVIDIA, 2=AMD, 3=both)");
    return result;
}

std::string H264Encoder::preset_to_string(VideoEncodingPreset preset) const {
    switch (preset) {
        case VideoEncodingPreset::ULTRAFAST:
            return "ultrafast";
        case VideoEncodingPreset::SUPERFAST:
            return "superfast";
        case VideoEncodingPreset::VERYFAST:
            return "veryfast";
        case VideoEncodingPreset::FASTER:
            return "faster";
        case VideoEncodingPreset::FAST:
            return "fast";
        case VideoEncodingPreset::MEDIUM:
            return "medium";
        case VideoEncodingPreset::SLOW:
            return "slow";
        case VideoEncodingPreset::SLOWER:
            return "slower";
        case VideoEncodingPreset::VERYSLOW:
            return "veryslow";
        case VideoEncodingPreset::PLACEBO:
            return "placebo";
        default:
            return "veryfast";
    }
}

AVCodecParameters* H264Encoder::get_codec_parameters() const {
    if (!codec_ctx_) {
        return nullptr;
    }

    AVCodecParameters* par = avcodec_parameters_alloc();
    if (!par) {
        return nullptr;
    }

    if (avcodec_parameters_from_context(par, codec_ctx_) < 0) {
        avcodec_parameters_free(&par);
        return nullptr;
    }

    return par;
}

AVRational H264Encoder::get_time_base() const {
    // ✅ FLV 容器统一使用毫秒作为 time_base (1/1000)
    // 视频和音频都用毫秒，保证音视频 PTS 可以直接比较
    return AVRational{1, 1000};
}

ErrorCode H264Encoder::initialize(const VideoEncoderConfig& config) {
    LOG_INFO("Initializing H.264 encoder (FFmpeg)");

    config_ = config;

    // 🔧 诊断：显示编码配置
    LOG_INFO("[H264Encoder] Config: prefer_hw=" + std::string(config_.prefer_hw ? "true" : "false") +
             ", hw_accel=" + std::to_string(static_cast<int>(config_.hw_accel)));

    // 🔧 强制第一帧为关键帧（I帧），解决视频开头卡顿问题
    // 这确保 RTMP 推流的第一帧视频可以被正确解码播放
    force_keyframe_ = true;

    // Prefer hardware encoder when requested and available.
    // We'll probe candidates in preferred order based on CPU vendor (Intel -> QSV first, AMD -> AMF first).
    codec_ = nullptr;
    if (config_.prefer_hw) {
        LOG_INFO("[H264Encoder] Hardware encoder requested, starting probe...");
        std::vector<const char*> candidates;

        // Detect CPU vendor on MSVC/GCC platforms where possible.
        std::string cpu_vendor;
#if defined(_MSC_VER) || defined(__GNUC__)
        {
#if defined(_MSC_VER)
            int regs[4] = {0};
            __cpuid(regs, 0);
            char vendor[13] = {0};
            *reinterpret_cast<int*>(vendor) = regs[1];
            *reinterpret_cast<int*>(vendor + 4) = regs[3];
            *reinterpret_cast<int*>(vendor + 8) = regs[2];
            cpu_vendor = std::string(vendor);
#elif defined(__GNUC__)
            unsigned int regs[4] = {0};
            __get_cpuid(0, &regs[0], &regs[1], &regs[2], &regs[3]);
            char vendor[13] = {0};
            *reinterpret_cast<unsigned int*>(vendor) = regs[1];
            *reinterpret_cast<unsigned int*>(vendor + 4) = regs[3];
            *reinterpret_cast<unsigned int*>(vendor + 8) = regs[2];
            cpu_vendor = std::string(vendor);
#endif
        }
#endif

        // Build candidates order based on GPU detection (智能选择策略)
        // 策略：无独显时 AMF(AMD集显) > x264 > QSV(Intel集显)
        //        有独显时 NVENC/AMF(独显) > x264 > QSV(集显)
        int gpu_type = detect_discrete_gpu();

        if (gpu_type & 1) {
            // NVIDIA 独显存在：优先使用 NVENC（独显），QSV 放 x264 后面
            candidates = {"h264_nvenc", "libx264", "h264_qsv"};
            LOG_INFO("[H264Encoder] NVIDIA dGPU detected → Priority: NVENC(dGPU) → Software → QSV(iGPU)");
        } else if (gpu_type & 2) {
            // AMD 独显存在：优先使用 AMF（独显），QSV 放 x264 后面
            candidates = {"h264_amf", "libx264", "h264_qsv"};
            LOG_INFO("[H264Encoder] AMD dGPU detected → Priority: AMF(dGPU) → Software → QSV(iGPU)");
        } else {
            // 无独显：AMD 集显 AMF > x264 > Intel QSV
            candidates = {"h264_amf", "libx264", "h264_qsv"};
            LOG_INFO("[H264Encoder] No discrete GPU → Priority: AMF(iGPU) → Software → QSV(iGPU)");
        }

        // 硬件编码器按优先级自动探测，支持回退到软件编码

        // Probe each candidate by trying to open a temporary codec context and checking codec parameters.
        for (size_t i = 0; i < candidates.size(); ++i) {
            const char* hw_name = candidates[i];
            LOG_INFO("[H264Encoder] Probing encoder: " + std::string(hw_name));

            const AVCodec* probe_codec = avcodec_find_encoder_by_name(hw_name);
            if (!probe_codec) {
                LOG_WARNING("[H264Encoder] Encoder '" + std::string(hw_name) + "' not found in FFmpeg");
                continue;
            }

            LOG_INFO("[H264Encoder] Found encoder '" + std::string(hw_name) + "', testing...");

            // create temporary context for probing
            AVCodecContext* probe_ctx = avcodec_alloc_context3(probe_codec);
            if (!probe_ctx) continue;
            probe_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
            probe_ctx->width = config_.width > 0 ? config_.width : 1280;
            probe_ctx->height = config_.height > 0 ? config_.height : 720;
            probe_ctx->time_base = AVRational{1, config_.fps > 0 ? config_.fps : 30};
            probe_ctx->framerate = AVRational{config_.fps > 0 ? config_.fps : 30, 1};

            // 🔧 QSV 分辨率要求检查
            if (std::string(hw_name).find("qsv") != std::string::npos) {
                int width = probe_ctx->width;
                int height = probe_ctx->height;
                LOG_INFO("[H264Encoder] QSV resolution check: " + std::to_string(width) + "x" + std::to_string(height));

                // QSV 要求宽度和高度必须是偶数
                if (width % 2 != 0 || height % 2 != 0) {
                    LOG_WARNING("[H264Encoder] ❌ QSV FAILED: Resolution must be even!");
                    LOG_WARNING("  Width: " + std::to_string(width) + (width % 2 != 0 ? " (odd) ❌" : " (even) ✓"));
                    LOG_WARNING("  Height: " + std::to_string(height) + (height % 2 != 0 ? " (odd) ❌" : " (even) ✓"));
                    LOG_WARNING("  → QSV requires both width and height to be even numbers");
                    avcodec_free_context(&probe_ctx);
                    continue;
                }

                // 某些 QSV 实现建议使用 16 对齐的分辨率
                if (width % 16 != 0 || height % 16 != 0) {
                    LOG_WARNING("[H264Encoder] ⚠ QSV resolution not 16-aligned:");
                    LOG_WARNING("  Width: " + std::to_string(width) + " (mod 16 = " + std::to_string(width % 16) + ")");
                    LOG_WARNING("  Height: " + std::to_string(height) + " (mod 16 = " + std::to_string(height % 16) + ")");
                    LOG_WARNING("  → Some QSV implementations prefer 16-aligned resolutions");
                } else {
                    LOG_INFO("[H264Encoder] ✓ Resolution is 16-aligned (optimal for QSV)");
                }

                // QSV 分辨率范围检查（Intel 集显典型限制）
                if (width < 128 || height < 128) {
                    LOG_WARNING("[H264Encoder] ⚠ Resolution may be too small for QSV (min recommended: 128x128)");
                }
                if (width > 4096 || height > 4096) {
                    LOG_WARNING("[H264Encoder] ⚠ Resolution may be too large for QSV (max depends on GPU)");
                }
            }

            // 🔧 根据编码器类型选择合适的像素格式
            // QSV 要求 NV12 格式，YUV420P 会导致 avcodec_open2 失败
            if (std::string(hw_name).find("qsv") != std::string::npos) {
                probe_ctx->pix_fmt = AV_PIX_FMT_NV12;  // QSV 必须使用 NV12
            } else {
                probe_ctx->pix_fmt = AV_PIX_FMT_YUV420P;  // NVENC/AMF/libx264 使用 YUV420P
            }

            probe_ctx->bit_rate = config_.bitrate;
            probe_ctx->gop_size = config_.gop > 0 ? config_.gop : (config_.fps > 0 ? config_.fps * 2 : 60);
            probe_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            // If probing QSV, try creating a qsv hwdevice and attach it to the probe context
            AVBufferRef* hw_device_ctx = nullptr;
            if (std::string(hw_name).find("qsv") != std::string::npos) {
                LOG_INFO("[H264Encoder] QSV encoder detected, creating hardware device...");
                LOG_INFO("[H264Encoder] Trying to create QSV device with child device type 'd3d11va'");

                // 🔧 尝试使用 d3d11va 作为 QSV 的子设备（Windows 上推荐）
                // 这样可以利用 Direct3D 11 的硬件加速
                int qsv_ret = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_QSV,
                                                      "d3d11va",  // 使用 d3d11va 作为实现
                                                      nullptr, 0);

                if (qsv_ret < 0 || !hw_device_ctx) {
                    LOG_WARNING("[H264Encoder] QSV+d3d11va failed, trying default QSV device...");
                    // 回退到默认的 QSV 设备（自动选择实现）
                    qsv_ret = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_QSV,
                                                          nullptr, nullptr, 0);
                }

                if (qsv_ret >= 0 && hw_device_ctx) {
                    // attach hwdevice to probe context
                    probe_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
                    LOG_INFO("[H264Encoder] QSV hardware device created successfully");
                } else {
                    if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
                    hw_device_ctx = nullptr;
                    char errbuf[128];
                    av_strerror(qsv_ret, errbuf, sizeof(errbuf));
                    LOG_WARNING("[H264Encoder] QSV hardware device creation failed: " + std::string(errbuf) +
                               " (code=" + std::to_string(qsv_ret) + ")");
                    LOG_WARNING("[H264Encoder] Common causes: 1) Intel GPU driver too old, 2) No Intel iGPU, 3) FFmpeg built without QSV");
                }
            }

            int ret = avcodec_open2(probe_ctx, probe_codec, nullptr);
            if (ret >= 0) {
                LOG_INFO("[H264Encoder] Opened encoder '" + std::string(hw_name) + "' successfully");
                AVCodecParameters* tmppar = avcodec_parameters_alloc();
                if (tmppar) {
                    if (avcodec_parameters_from_context(tmppar, probe_ctx) >= 0) {
                        // candidate works -> select it
                        codec_ = probe_codec;
                        // ✅ 保存编码器候选列表和当前索引
                        encoder_candidates_.clear();
                        for (const auto& c : candidates) {
                            encoder_candidates_.push_back(std::string(c));
                        }
                        current_encoder_index_ = static_cast<int>(i);
                        // ✅ 标记是否为 QSV 编码器
                        is_qsv_encoder_ = (std::string(hw_name).find("qsv") != std::string::npos);
                        avcodec_parameters_free(&tmppar);
                        avcodec_free_context(&probe_ctx);
                        if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
                        LOG_INFO(std::string("Selected encoder after probe: ") + hw_name +
                                " (index " + std::to_string(current_encoder_index_) + "/" +
                                std::to_string(encoder_candidates_.size()) + ")");
                        break;
                    }
                    avcodec_parameters_free(&tmppar);
                    LOG_WARNING("[H264Encoder] Encoder '" + std::string(hw_name) + "' opened but codec parameters extraction failed");
                }
            } else {
                char errbuf[128];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_WARNING("[H264Encoder] Failed to open encoder '" + std::string(hw_name) +
                           "': " + std::string(errbuf) + " (code=" + std::to_string(ret) + ")");

                // 🔧 为QSV添加更详细的诊断信息
                if (std::string(hw_name).find("qsv") != std::string::npos) {
                    LOG_WARNING("[H264Encoder] QSV specific diagnostics:");
                    LOG_WARNING("  - Input pixel format: " + std::string(av_get_pix_fmt_name(probe_ctx->pix_fmt)));
                    LOG_WARNING("  - Required format for QSV: NV12 (or P010 for 10-bit)");
                    LOG_WARNING("  - Check if Intel GPU driver supports QSV encoding");
                    LOG_WARNING("  - Try installing Intel Media SDK for better QSV support");
                }
            }
            // cleanup and continue
            avcodec_free_context(&probe_ctx);
            if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
        }

        if (!codec_) {
            LOG_WARNING("[H264Encoder] No suitable hardware encoder found after probing");
            LOG_INFO("[H264Encoder] Will fallback to software encoder (libx264)");
        } else {
            LOG_INFO("[H264Encoder] Hardware encoder probe completed successfully");
        }
    }

    // Fallback to software encoder (libx264) if no hardware encoder was selected/found
    if (!codec_) {
        LOG_INFO("[H264Encoder] ========================================");
        LOG_INFO("[H264Encoder] All hardware encoders failed!");
        LOG_INFO("[H264Encoder] Probing sequence: QSV -> AMF -> NVENC");
        LOG_INFO("[H264Encoder] Fallback to software encoder (libx264)");
        LOG_INFO("[H264Encoder] ========================================");
        codec_ = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec_) {
            LOG_ERROR("Failed to find H264 encoder");
            return ErrorCode::INIT_FAILED;
        }
        LOG_INFO(std::string("[H264Encoder] Using software encoder: ") + codec_->name);
    } else {
        // 显示最终选择的硬件编码器
        LOG_INFO("[H264Encoder] ========================================");
        LOG_INFO(std::string("[H264Encoder] Hardware encoder selected: ") + codec_->name);
        LOG_INFO("[H264Encoder] ========================================");
    }

    codec_ctx_ = avcodec_alloc_context3(codec_);
    if (!codec_ctx_) {
        LOG_ERROR("Failed to alloc H264 codec context");
        return ErrorCode::INIT_FAILED;
    }

    codec_ctx_->codec_type = AVMEDIA_TYPE_VIDEO;
    codec_ctx_->codec_id = AV_CODEC_ID_H264;
    codec_ctx_->width = config_.width;
    codec_ctx_->height = config_.height;
    codec_ctx_->time_base = get_time_base();
    codec_ctx_->framerate = AVRational{config_.fps > 0 ? config_.fps : 30, 1};

    // ✅ 降低编码延迟的关键设置
    codec_ctx_->max_b_frames = 0;  // 禁用 B 帧
    // 硬件编码器(QSV/NVENC/AMF)不支持FFmpeg软件线程切片，保持默认
    // 软件编码器(libx264)通过x264-params配置自己的多线程
    codec_ctx_->thread_count = 0;  // 0 = 让编码器自行决定
    codec_ctx_->thread_type = 0;   // 由编码器自行决定

    // ✅ GOP: 固定 2 秒关键帧间隔
    int gop_size = config_.gop > 0 ? config_.gop : (config_.fps > 0 ? config_.fps * 2 : 60);
    codec_ctx_->gop_size = gop_size;
    codec_ctx_->keyint_min = config_.fps;  // 最小关键帧间隔 = 1秒

    LOG_INFO("[H264Encoder] GOP size set to " + std::to_string(gop_size) + " frames (" +
             std::to_string(gop_size / (config_.fps > 0 ? config_.fps : 30)) + " seconds @ " +
             std::to_string(config_.fps) + " fps)");

    // 🔧 像素格式设置
    if (is_qsv_encoder_) {
        // QSV 编码器需要使用 AV_PIX_FMT_QSV 格式
        // 实际的像素格式（NV12）在硬件帧上下文中指定
        codec_ctx_->pix_fmt = AV_PIX_FMT_QSV;
        LOG_INFO("[H264Encoder] Using QSV pixel format (hardware acceleration)");

        // ✅ 创建 QSV 硬件设备上下文
        // Phase 4: 首先尝试从 SharedD3D11Device 派生（共享同一 D3D11 设备，支持 GPU→GPU 零拷贝）
        if (!hw_device_ctx_) {
            bool shared_ok = false;
            if (!d3d11va_device_ctx_) {
                shared_ok = initialize_shared_d3d11va();
            } else {
                shared_ok = true;
            }

            if (shared_ok && d3d11va_device_ctx_) {
                int ret = av_hwdevice_ctx_create_derived(
                    &hw_device_ctx_, AV_HWDEVICE_TYPE_QSV, d3d11va_device_ctx_, 0);
                if (ret >= 0) {
                    LOG_INFO("[H264Encoder] QSV device derived from shared D3D11VA (Phase 4 GPU path enabled)");
                } else {
                    char errbuf[128];
                    av_strerror(ret, errbuf, sizeof(errbuf));
                    LOG_WARNING("[H264Encoder] QSV derive from shared D3D11VA failed: " +
                                std::string(errbuf) + " — falling back to independent QSV device");
                    av_buffer_unref(&d3d11va_device_ctx_);
                }
            }

            // 回退：独立 QSV 设备（与以往行为相同）
            if (!hw_device_ctx_) {
                int ret = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_QSV,
                                                "d3d11va", nullptr, 0);
                if (ret < 0) {
                    LOG_WARNING("[H264Encoder] QSV+d3d11va failed, trying default...");
                    ret = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_QSV,
                                                nullptr, nullptr, 0);
                }
                if (ret < 0) {
                    LOG_ERROR("[H264Encoder] Failed to create QSV device context");
                    return ErrorCode::INIT_FAILED;
                }
                LOG_INFO("[H264Encoder] QSV hardware device context created (independent device)");
            }
        }

        // ✅ 创建 QSV 硬件帧上下文（指定实际像素格式为 NV12）
        hw_frame_ctx_ = av_hwframe_ctx_alloc(hw_device_ctx_);
        if (!hw_frame_ctx_) {
            LOG_ERROR("[H264Encoder] Failed to allocate QSV hwframe context");
            return ErrorCode::INIT_FAILED;
        }

        AVHWFramesContext* frames_ctx = (AVHWFramesContext*)hw_frame_ctx_->data;
        frames_ctx->format = AV_PIX_FMT_QSV;
        frames_ctx->sw_format = AV_PIX_FMT_NV12;  // QSV 要求 NV12 格式
        frames_ctx->width = codec_ctx_->width;
        frames_ctx->height = codec_ctx_->height;
        frames_ctx->initial_pool_size = 20;  // 帧池大小

        int ret = av_hwframe_ctx_init(hw_frame_ctx_);
        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("[H264Encoder] Failed to initialize QSV hwframe context: " + std::string(errbuf));
            return ErrorCode::INIT_FAILED;
        }

        // 将硬件帧上下文附加到编码器
        codec_ctx_->hw_frames_ctx = av_buffer_ref(hw_frame_ctx_);
        LOG_INFO("[H264Encoder] QSV hwframe context initialized (NV12, "
                + std::to_string(config_.width) + "x" + std::to_string(config_.height) + ")");
    } else {
        // 其他编码器使用 YUV420P
        codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
        LOG_INFO("[H264Encoder] Using YUV420P pixel format");
    }

    // For FLV/RTMP, extradata is typically required
    codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // 🔧 硬件编码器公共配置：强制 avcC 格式（非 Annex-B）
    // NVENC/AMF 默认可能生成 Annex-B 格式的 extradata（start code 00 00 00 01），
    // FLV/RTMP 容器要求 avcC 格式（length-prefixed NAL units，首字节 0x01）
    if (codec_ctx_->priv_data && codec_) {
        std::string enc_name = codec_->name;

        if (enc_name.find("nvenc") != std::string::npos) {
            // NVENC: 强制 avcC 输出格式（设置 h264_demuxer 为 avcC）
            av_opt_set(codec_ctx_->priv_data, "h264_demuxer", "avc", 0);
            // 低延迟配置
            av_opt_set(codec_ctx_->priv_data, "delay", "0", 0);
            av_opt_set(codec_ctx_->priv_data, "tune", "ll", 0);
            codec_ctx_->max_b_frames = 0;
            LOG_INFO("H264Encoder: configured NVENC: avcC format, low-latency");
        } else if (enc_name.find("amf") != std::string::npos) {
            // AMF: usage=transcoding 产出 avcC 格式 extradata
            av_opt_set(codec_ctx_->priv_data, "usage", "transcoding", 0);
            codec_ctx_->max_b_frames = 0;
            LOG_INFO("H264Encoder: configured AMF: avcC format (usage=transcoding)");
        } else if (enc_name.find("qsv") != std::string::npos) {
            // QSV: h264_demuxer=avc 强制 avcC 格式 extradata
            av_opt_set(codec_ctx_->priv_data, "h264_demuxer", "avc", 0);
            codec_ctx_->max_b_frames = 0;
            LOG_INFO("H264Encoder: configured QSV: avcC format (h264_demuxer=avc)");
        }
    }

    // x264 options (works when underlying encoder is libx264)
    // ✅ 参考原项目 qt-live-client 的配置
    if (codec_ctx_->priv_data) {
        av_opt_set(codec_ctx_->priv_data, "preset", preset_to_string(config_.preset).c_str(), 0);
        av_opt_set(codec_ctx_->priv_data, "tune", "zerolatency", 0);

        // Only set x264-specific params when using libx264 (software) encoder.
        if (codec_ && std::string(codec_->name).find("libx264") != std::string::npos) {
            // ✅ 使用简单可靠的 x264-params
            // 注意：不要设置 slice-max-size 和 slices 参数，让编码器自动处理
            // 设置过小的 slice-max-size 会导致 slice 数量过多，服务器解码器无法处理
            std::string x264_params = "ref=1:profile=baseline";

            // ✅ 禁用场景检测，严格按照 GOP 间隔生成 IDR 关键帧
            // scenecut=0 表示禁用场景检测，确保第一帧和每 GOP 帧都生成 IDR
            x264_params += ":scenecut=0";

            // ✅ 强制第一帧为 IDR 帧，这是解决音视频同步问题的关键
            // 没有这个参数，第一个 I 帧可能不是 IDR，会导致推流时大量丢帧
            x264_params += ":forced-idr=1";

            // ✅ 使用多线程编码提升吞吐量
            x264_params += ":threads=4";

            // Force AVCC output (length-prefixed NAL units) for FLV/RTMP muxing.
            // This avoids Annex-B start codes leaking into downstream packet handling.
            x264_params += ":annexb=0";
            x264_params += ":repeat-headers=0";

            av_opt_set(codec_ctx_->priv_data, "x264-params", x264_params.c_str(), 0);
            LOG_INFO(std::string("H264Encoder: set x264-params: ") + x264_params);
        } else {
            LOG_INFO("H264Encoder: codec not libx264, skipping x264-params configuration");
        }
    }

    // ✅ 参考原项目：使用 AVDictionary 设置码率控制参数
    AVDictionary* opts = nullptr;
    if (codec_ && std::string(codec_->name).find("libx264") != std::string::npos) {
        // 软件编码器：设置 VBR 码率控制
        av_dict_set(&opts, "rc", "vbr", 0);
        av_dict_set(&opts, "b", (std::to_string(config_.bitrate / 1000) + "k").c_str(), 0);
        av_dict_set(&opts, "maxrate", (std::to_string(config_.max_bitrate / 1000) + "k").c_str(), 0);
        av_dict_set(&opts, "bufsize", (std::to_string((config_.max_bitrate / 1000) * 2) + "k").c_str(), 0);
        av_dict_set(&opts, "max_delay", "20000000", 0);
        LOG_INFO("[H264Encoder] Set libx264 bitrate: " + std::to_string(config_.bitrate / 1000) + "k, maxrate: " +
                 std::to_string(config_.max_bitrate / 1000) + "k");
    }

    if (avcodec_open2(codec_ctx_, codec_, &opts) < 0) {
        av_dict_free(&opts);
        LOG_ERROR("Failed to open H264 encoder");
        return ErrorCode::INIT_FAILED;
    }
    av_dict_free(&opts);

    // ✅ 强制首帧为 IDR 关键帧
    force_keyframe_ = true;
    LOG_INFO("[H264Encoder] First frame will be forced as keyframe (IDR)");

    // ✅ 只为非 QSV 编码器创建软件帧
    // QSV 编码器使用硬件帧，在 send_frame_internal 中创建
    if (!is_qsv_encoder_) {
        frame_ = av_frame_alloc();
        if (!frame_) {
            LOG_ERROR("Failed to alloc video frame");
            return ErrorCode::INIT_FAILED;
        }

        frame_->format = codec_ctx_->pix_fmt;
        frame_->width = codec_ctx_->width;
        frame_->height = codec_ctx_->height;

        if (av_frame_get_buffer(frame_, 32) < 0) {
            LOG_ERROR("Failed to alloc frame buffer");
            return ErrorCode::INIT_FAILED;
        }
    }

    // Input pixel format may vary (RGBA/NV12). Initialize with RGBA; will be recreated on demand.
    AVPixelFormat target_fmt = is_qsv_encoder_ ? AV_PIX_FMT_NV12 : codec_ctx_->pix_fmt;
    sws_ctx_ = sws_getContext(
        config_.width,
        config_.height,
        AV_PIX_FMT_RGBA,
        config_.width,
        config_.height,
        target_fmt,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr);

    if (!sws_ctx_) {
        LOG_ERROR("Failed to create sws context");
        return ErrorCode::INIT_FAILED;
    }

    initialized_ = true;

    // 🔧 显示最终使用的编码器
    std::string encoder_type = codec_->name;
    bool is_hw = (encoder_type.find("qsv") != std::string::npos ||
                 encoder_type.find("nvenc") != std::string::npos ||
                 encoder_type.find("amf") != std::string::npos);

    LOG_INFO("H264 encoder initialized: " + std::to_string(config_.width) + "x" + std::to_string(config_.height) +
             " fps=" + std::to_string(config_.fps) + " bitrate=" + std::to_string(config_.bitrate));
    LOG_INFO("[H264Encoder] Using encoder: " + encoder_type +
             " (" + (is_hw ? "Hardware" : "Software") + ")");

    // 🔧 诊断：检查 extradata (SPS/PPS) 是否正确生成
    if (codec_ctx_->extradata && codec_ctx_->extradata_size > 0) {
        LOG_INFO("[H264Encoder] extradata generated: size=" + std::to_string(codec_ctx_->extradata_size));
        // 打印前几个字节用于调试（avcC 格式第一字节应该是 0x01）
        std::string hex;
        for (int i = 0; i < std::min(codec_ctx_->extradata_size, 20); i++) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02X ", codec_ctx_->extradata[i]);
            hex += buf;
        }
        LOG_INFO("[H264Encoder] extradata (first 20 bytes): " + hex);

        // 验证 avcC 格式
        if (codec_ctx_->extradata[0] == 0x01) {
            LOG_INFO("[H264Encoder] ✓ extradata is valid avcC format (first byte = 0x01)");
        } else if (codec_ctx_->extradata[0] == 0x00 && codec_ctx_->extradata_size >= 4 &&
                   codec_ctx_->extradata[1] == 0x00 && codec_ctx_->extradata[2] == 0x00) {
            LOG_WARNING("[H264Encoder] ✗ extradata appears to be Annex-B format (start code detected)!");
            LOG_WARNING("[H264Encoder]   This may cause issues with FLV/RTMP muxing!");
        } else {
            LOG_WARNING("[H264Encoder] ? extradata format unknown (first byte = 0x" +
                       std::to_string(codec_ctx_->extradata[0]) + ")");
        }
    } else {
        LOG_WARNING("[H264Encoder] WARNING: No extradata generated! This may cause issues with FLV/RTMP.");
    }

    // 🔧 检查 AV_CODEC_FLAG_GLOBAL_HEADER 标志
    bool has_global_header = (codec_ctx_->flags & AV_CODEC_FLAG_GLOBAL_HEADER) != 0;
    LOG_INFO("[H264Encoder] AV_CODEC_FLAG_GLOBAL_HEADER: " + std::to_string(has_global_header));

    return ErrorCode::SUCCESS;
}

ErrorCode H264Encoder::shutdown() {
    initialized_ = false;

    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }

    // ✅ 释放硬件帧资源
    if (hw_frame_) {
        av_frame_free(&hw_frame_);
        hw_frame_ = nullptr;
    }

    if (sw_frame_) {
        av_frame_free(&sw_frame_);
        sw_frame_ = nullptr;
    }

    if (hw_frame_ctx_) {
        av_buffer_unref(&hw_frame_ctx_);
        hw_frame_ctx_ = nullptr;
    }

    if (hw_device_ctx_) {
        av_buffer_unref(&hw_device_ctx_);
        hw_device_ctx_ = nullptr;
    }

    // Phase 4: 释放 D3D11VA 上下文（在 hw_device_ctx_ 之后，因为 QSV 可能持有对它的引用）
    if (d3d11va_frame_ctx_) {
        av_buffer_unref(&d3d11va_frame_ctx_);
        d3d11va_frame_ctx_ = nullptr;
    }
    if (d3d11va_device_ctx_) {
        av_buffer_unref(&d3d11va_device_ctx_);
        d3d11va_device_ctx_ = nullptr;
    }

    if (frame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }

    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }

    codec_ = nullptr;
    force_keyframe_ = false;
    first_keyframe_sent_ = false;  // 🔧 重置第一帧标记
    is_qsv_encoder_ = false;

    return ErrorCode::SUCCESS;
}

ErrorCode H264Encoder::send_frame_internal(const std::shared_ptr<VideoFrame>& in) {
    if (!in || !codec_ctx_) {
        return ErrorCode::INVALID_PARAM;
    }

    // 对于QSV编码器，检查硬件帧上下文
    if (is_qsv_encoder_ && !hw_frame_ctx_) {
        LOG_ERROR("[H264Encoder] QSV encoder requires hw_frame_ctx_");
        return ErrorCode::ENCODING_ERROR;
    }

    AVPixelFormat src_fmt = AV_PIX_FMT_RGBA;
    if (in->format == VideoFrame::PixelFormat::NV12) {
        src_fmt = AV_PIX_FMT_NV12;
    }

    // 🔧 对于 QSV 编码器，需要使用硬件帧
    AVFrame* encode_frame = nullptr;
    if (is_qsv_encoder_) {
        // ✅ QSV 编码器：创建硬件帧并上传数据
        if (!hw_frame_) {
            hw_frame_ = av_frame_alloc();
            if (!hw_frame_) {
                LOG_ERROR("[H264Encoder] Failed to allocate hw frame");
                return ErrorCode::ENCODING_ERROR;
            }
        }

        // 释放上一帧对帧池 buffer 的引用，否则帧池（默认 20 帧）会在第 21 帧时耗尽
        av_frame_unref(hw_frame_);

        // 从硬件帧池获取一个帧
        int ret = av_hwframe_get_buffer(hw_frame_ctx_, hw_frame_, 0);
        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("[H264Encoder] Failed to get hwframe buffer: " + std::string(errbuf));
            return ErrorCode::ENCODING_ERROR;
        }

        // 创建软件帧用于转换（NV12 格式）
        if (!sw_frame_) {
            sw_frame_ = av_frame_alloc();
            if (!sw_frame_) {
                LOG_ERROR("[H264Encoder] Failed to allocate sw frame");
                return ErrorCode::ENCODING_ERROR;
            }
            sw_frame_->format = AV_PIX_FMT_NV12;
            sw_frame_->width = config_.width;
            sw_frame_->height = config_.height;
            ret = av_frame_get_buffer(sw_frame_, 0);
            if (ret < 0) {
                LOG_ERROR("[H264Encoder] Failed to allocate sw frame buffer");
                return ErrorCode::ENCODING_ERROR;
            }
        }

        // 将输入数据写入软件帧（NV12）
        if (in->format == VideoFrame::PixelFormat::NV12) {
            // NV12 直接 memcpy：避免 sws NV12→NV12 因内部中间格式转换导致色彩偏差（绿屏）
            // Y 平面：逐行复制，处理 src/dst stride 不同的情况
            const int copy_w = std::min(in->width, sw_frame_->width);
            const int copy_h = std::min(in->height, sw_frame_->height);
            for (int y = 0; y < copy_h; ++y) {
                memcpy(sw_frame_->data[0] + y * sw_frame_->linesize[0],
                       in->data.get() + y * in->stride, copy_w);
            }
            // UV 平面（NV12 interleaved，行数为 height/2，每行字节数与 Y 相同）
            for (int y = 0; y < copy_h / 2; ++y) {
                memcpy(sw_frame_->data[1] + y * sw_frame_->linesize[1],
                       in->data_uv.get() + y * in->stride_uv, copy_w);
            }
        } else {
            // 非 NV12 输入：使用 sws 转换
            AVPixelFormat target_fmt = AV_PIX_FMT_NV12;
            if (!sws_ctx_ || sws_src_fmt_ != src_fmt) {
                if (sws_ctx_) {
                    sws_freeContext(sws_ctx_);
                    sws_ctx_ = nullptr;
                }
                sws_ctx_ = sws_getContext(
                    config_.width, config_.height, src_fmt,
                    config_.width, config_.height, target_fmt,
                    SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!sws_ctx_) {
                    LOG_ERROR("[H264Encoder] Failed to create sws context for QSV");
                    return ErrorCode::ENCODING_ERROR;
                }
                sws_src_fmt_ = src_fmt;
            }
            const uint8_t* src_slices[1] = {reinterpret_cast<const uint8_t*>(in->data.get())};
            int src_stride[1] = {in->stride > 0 ? in->stride : in->width * 4};
            sws_scale(sws_ctx_, src_slices, src_stride, 0, in->height,
                     sw_frame_->data, sw_frame_->linesize);
        }

        // 上传到 GPU（硬件帧）
        ret = av_hwframe_transfer_data(hw_frame_, sw_frame_, 0);
        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("[H264Encoder] Failed to transfer frame to GPU: " + std::string(errbuf));
            return ErrorCode::ENCODING_ERROR;
        }

        encode_frame = hw_frame_;

    } else {
        // ✅ 软件编码器：使用原来的逻辑
        if (!frame_) {
            LOG_ERROR("[H264Encoder] Frame not initialized");
            return ErrorCode::ENCODING_ERROR;
        }

        if (av_frame_make_writable(frame_) < 0) {
            return ErrorCode::ENCODING_ERROR;
        }

        // 软件编码器的像素格式
        AVPixelFormat target_fmt = codec_ctx_->pix_fmt;
        if (!sws_ctx_ || sws_src_fmt_ != src_fmt || sws_src_w_ != in->width || sws_src_h_ != in->height) {
            if (sws_ctx_) {
                sws_freeContext(sws_ctx_);
                sws_ctx_ = nullptr;
            }

            sws_ctx_ = sws_getContext(
                config_.width, config_.height, src_fmt,
                config_.width, config_.height, target_fmt,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!sws_ctx_) {
                LOG_ERROR("Failed to create sws context (dynamic)");
                return ErrorCode::ENCODING_ERROR;
            }
            sws_src_fmt_ = src_fmt;
            sws_src_w_ = in->width;
            sws_src_h_ = in->height;
        }

        if (in->format == VideoFrame::PixelFormat::NV12) {
            const uint8_t* src_slices[2] = {in->data.get(), in->data_uv.get()};
            int src_stride[2] = {in->stride, in->stride_uv};
            sws_scale(sws_ctx_, src_slices, src_stride, 0, in->height,
                     frame_->data, frame_->linesize);
        } else {
            const uint8_t* src_slices[1] = {reinterpret_cast<const uint8_t*>(in->data.get())};
            int src_stride[1] = {in->stride > 0 ? in->stride : in->width * 4};
            sws_scale(sws_ctx_, src_slices, src_stride, 0, in->height,
                     frame_->data, frame_->linesize);
        }

        encode_frame = frame_;
    }

    encode_frame->pts = in->timestamp_ms;

    if (force_keyframe_) {
        encode_frame->pict_type = AV_PICTURE_TYPE_I;
        encode_frame->key_frame = 1;
        force_keyframe_ = false;
        LOG_INFO("[H264Encoder] Force IDR frame at pts=" + std::to_string(in->timestamp_ms));
    } else {
        encode_frame->pict_type = AV_PICTURE_TYPE_NONE;
        encode_frame->key_frame = 0;
    }

    int ret = avcodec_send_frame(codec_ctx_, encode_frame);
    if (ret < 0) {
        char errbuf[1024];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("[H264Encoder] avcodec_send_frame failed: " + std::string(errbuf));
        LOG_ERROR("[H264Encoder] Encoder: " + std::string(codec_->name));
        if (encode_frame) {
            LOG_ERROR("[H264Encoder] Frame info: " + std::to_string(encode_frame->width) + "x" + std::to_string(encode_frame->height) +
                     ", format=" + av_get_pix_fmt_name(static_cast<AVPixelFormat>(encode_frame->format)) +
                     ", pts=" + std::to_string(encode_frame->pts));
        }

        // 如果是硬件编码器且不是软件编码器，记录这个错误可能需要切换
        if (!has_exhausted_encoders_ && std::string(codec_->name).find("libx264") == std::string::npos) {
            LOG_ERROR("[H264Encoder] This is a hardware encoder error - may trigger fallback to next encoder");
        }

        return ErrorCode::ENCODING_ERROR;
    }

    return ErrorCode::SUCCESS;
}

ErrorCode H264Encoder::send_flush() {
    if (!codec_ctx_) {
        return ErrorCode::INIT_FAILED;
    }

    if (avcodec_send_frame(codec_ctx_, nullptr) < 0) {
        LOG_ERROR("avcodec_send_frame(flush) failed");
        return ErrorCode::ENCODING_ERROR;
    }

    return ErrorCode::SUCCESS;
}

// ─── Annex-B → AVCC 包体归一化 ───────────────────────────────────────────────
// QSV（Intel Quick Sync）即使设置了 AV_CODEC_FLAG_GLOBAL_HEADER，硬件层输出的
// 包体仍携带 start code（Annex-B 字节流格式）。FLV/RTMP 要求 AVCC 格式（4 字节
// 长度前缀），因此在编码器出口做一次统一转换，避免下游 muxer/pusher 各自处理。
//
// 转换规则：
//   Annex-B:  [00 00 00 01][NAL data][00 00 00 01][NAL data]...
//   AVCC:     [4字节大端长度][NAL data][4字节大端长度][NAL data]...
// ─────────────────────────────────────────────────────────────────────────────
static bool normalize_h264_packet_to_avcc(AVPacket* pkt)
{
    if (!pkt || !pkt->data || pkt->size < 4) return true;

    const uint8_t* d = pkt->data;
    // 如果首字节非零，肯定不是 Annex-B start code，直接跳过
    if (d[0] != 0) return true;
    // 4字节 start code: 00 00 00 01
    bool is_annexb = (d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 1);
    // 3字节 start code: 00 00 01（前面可能有 00 填充）
    if (!is_annexb) is_annexb = (d[0] == 0 && d[1] == 0 && d[2] == 1);
    if (!is_annexb) return true;  // 不是 Annex-B，无需转换

    const uint8_t* src  = pkt->data;
    const int      size = pkt->size;
    std::vector<uint8_t> avcc;
    avcc.reserve(size);

    int i = 0;
    int nal_count = 0;
    while (i < size) {
        // 找 start code
        int sc_len = 0;
        if (i + 3 < size && src[i]==0 && src[i+1]==0 && src[i+2]==0 && src[i+3]==1)
            sc_len = 4;
        else if (i + 2 < size && src[i]==0 && src[i+1]==0 && src[i+2]==1)
            sc_len = 3;
        else { ++i; continue; }

        const int nal_start = i + sc_len;
        if (nal_start >= size) break;

        // 找下一个 start code 作为结束边界
        int nal_end = size;
        for (int j = nal_start; j < size - 2; ++j) {
            if (src[j]==0 && src[j+1]==0) {
                if (src[j+2] == 1) { nal_end = j; break; }
                if (j + 3 < size && src[j+2]==0 && src[j+3]==1) { nal_end = j; break; }
            }
        }

        int nal_size = nal_end - nal_start;
        if (nal_size <= 0) { i = nal_end; continue; }

        // 写 4 字节大端长度 + NAL 数据
        avcc.push_back(static_cast<uint8_t>((nal_size >> 24) & 0xFF));
        avcc.push_back(static_cast<uint8_t>((nal_size >> 16) & 0xFF));
        avcc.push_back(static_cast<uint8_t>((nal_size >>  8) & 0xFF));
        avcc.push_back(static_cast<uint8_t>( nal_size        & 0xFF));
        avcc.insert(avcc.end(), src + nal_start, src + nal_end);
        ++nal_count;
        i = nal_end;
    }

    if (nal_count == 0 || avcc.empty()) return false;

    // 保存 av_packet_unref 会清零的元数据
    const int64_t saved_pts      = pkt->pts;
    const int64_t saved_dts      = pkt->dts;
    const int64_t saved_duration = pkt->duration;
    const int      saved_flags   = pkt->flags;
    const int      saved_sidx    = pkt->stream_index;

    av_packet_unref(pkt);
    if (av_new_packet(pkt, static_cast<int>(avcc.size())) < 0) return false;
    memcpy(pkt->data, avcc.data(), avcc.size());
    pkt->pts          = saved_pts;
    pkt->dts          = saved_dts;
    pkt->duration     = saved_duration;
    pkt->flags        = saved_flags;
    pkt->stream_index = saved_sidx;
    return true;
}

ErrorCode H264Encoder::receive_packets(std::vector<EncodedPacketPtr>& packets) {
    packets.clear();

    if (!codec_ctx_) {
        return ErrorCode::INIT_FAILED;
    }

    while (true) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) {
            return ErrorCode::INIT_FAILED;
        }

        int ret = avcodec_receive_packet(codec_ctx_, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&pkt);
            break;
        }
        if (ret < 0) {
            av_packet_free(&pkt);
            LOG_ERROR("avcodec_receive_packet failed");
            return ErrorCode::ENCODING_ERROR;
        }

        // Ensure packet is reference-counted so its data buffer isn't freed unexpectedly
        if (av_packet_make_refcounted(pkt) < 0) {
            LOG_WARNING("av_packet_make_refcounted failed for received packet; proceeding but this may risk buffer lifetime issues");
        }

        // QSV 硬件编码器输出可能带 Annex-B start code，在此归一化为 AVCC
        //if (!normalize_h264_packet_to_avcc(pkt)) {
        //    LOG_WARNING("[H264Encoder] Failed to normalize packet to AVCC, dropping");
        //    av_packet_free(&pkt);
        //    continue;
        //}

        // 🔧 修复：强制第一帧为关键帧（IDR）
        // 即使 x264 参数设置了 forced-idr=1，编码器输出可能仍不包含关键帧标志
        // 这里我们强制第一帧为关键帧，确保 RTMP 推流不会丢弃第一帧视频
        if (!first_keyframe_sent_) {
            pkt->flags |= AV_PKT_FLAG_KEY;
            first_keyframe_sent_ = true;
            LOG_INFO("[H264Encoder] Force first encoded frame as keyframe, pts=" + std::to_string(pkt->pts));
        }

        auto out = std::make_shared<EncodedPacket>();
        out->type = MediaType::VIDEO;
        out->pts = pkt->pts;
        out->dts = pkt->dts;
        // x264 通常不对每个 packet 填 duration，导致帧时长为 0，进而让 HLS/m3u8 播放器卡住
        // 兜底：用 fps 推算一帧的毫秒时长（整数），与音频 encoder 策略一致
        out->duration = (pkt->duration > 0) ? pkt->duration
                                           : (config_.fps > 0 ? (1000 / config_.fps) : 33);
        out->encoder_time_base = get_time_base();
        out->is_keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        out->priority = 1;
        out->pkt = AVPacketPtr(pkt);

        packets.push_back(std::move(out));
    }

    return ErrorCode::SUCCESS;
}

// ─── Phase 4: GPU 纹理直接编码 ────────────────────────────────────────────────
// 完全跳过 CPU：无 sws_scale，无 av_hwframe_transfer_data（CPU→GPU upload）
// 流程：GpuColorConverter NV12 纹理 → av_hwframe_map 解包 QSV 帧的 D3D11 纹理
//        → D3D11 CopySubresourceRegion（纯 GPU Copy）→ avcodec_send_frame

ErrorCode H264Encoder::encode_gpu_texture(ID3D11Texture2D* nv12_texture, int64_t pts_ms,
                                            std::vector<EncodedPacketPtr>& packets)
{
    packets.clear();

    if (!initialized_ || !nv12_texture) return ErrorCode::INVALID_PARAM;

    if (!is_qsv_encoder_ || !hw_frame_ctx_ || !d3d11va_device_ctx_) {
        LOG_ERROR("[H264Encoder] encode_gpu_texture requires QSV encoder with shared D3D11VA device");
        return ErrorCode::INVALID_PARAM;
    }

    // ── 1. 懒初始化 D3D11VA 帧上下文（供 av_hwframe_map 使用）──────────────
    if (!d3d11va_frame_ctx_) {
        d3d11va_frame_ctx_ = av_hwframe_ctx_alloc(d3d11va_device_ctx_);
        if (!d3d11va_frame_ctx_) {
            LOG_ERROR("[H264Encoder] av_hwframe_ctx_alloc(D3D11VA frames) failed");
            return ErrorCode::ENCODING_ERROR;
        }
        auto* fc        = reinterpret_cast<AVHWFramesContext*>(d3d11va_frame_ctx_->data);
        fc->format      = AV_PIX_FMT_D3D11;
        fc->sw_format   = AV_PIX_FMT_NV12;
        fc->width       = config_.width;
        fc->height      = config_.height;
        fc->initial_pool_size = 1;  // 最小池，满足 init 要求；实际使用 av_hwframe_map 映射 QSV 帧

        int r = av_hwframe_ctx_init(d3d11va_frame_ctx_);
        if (r < 0) {
            char errbuf[128]; av_strerror(r, errbuf, sizeof(errbuf));
            LOG_ERROR("[H264Encoder] D3D11VA frame ctx init failed: " + std::string(errbuf));
            av_buffer_unref(&d3d11va_frame_ctx_);
            return ErrorCode::ENCODING_ERROR;
        }
        LOG_INFO("[H264Encoder] D3D11VA frames context initialized (for av_hwframe_map)");
    }

    // ── 2. 从 QSV 帧池获取一帧 ───────────────────────────────────────────────
    if (!hw_frame_) {
        hw_frame_ = av_frame_alloc();
        if (!hw_frame_) return ErrorCode::ENCODING_ERROR;
    }
    // 释放上一帧对帧池 buffer 的引用，否则帧池会在第 21 帧时耗尽
    av_frame_unref(hw_frame_);
    int ret = av_hwframe_get_buffer(hw_frame_ctx_, hw_frame_, 0);
    if (ret < 0) {
        char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("[H264Encoder] av_hwframe_get_buffer failed: " + std::string(errbuf));
        return ErrorCode::ENCODING_ERROR;
    }

    // ── 3. 将 QSV 帧映射为 D3D11 纹理（通过 D3D11VA interop）───────────────
    // av_hwframe_map: QSV → D3D11（要求 QSV 从 D3D11VA 派生）
    AVFrame* mapped = av_frame_alloc();
    if (!mapped) return ErrorCode::ENCODING_ERROR;

    mapped->format = AV_PIX_FMT_D3D11;
    // hw_frames_ctx 必须在 av_hwframe_map 之前设置，否则 FFmpeg QSV→D3D11 映射
    // 无法确定目标帧上下文，导致 data[0]/data[1] 未正确填充
    mapped->hw_frames_ctx = av_buffer_ref(d3d11va_frame_ctx_);

    ret = av_hwframe_map(mapped, hw_frame_,
                         AV_HWFRAME_MAP_WRITE | AV_HWFRAME_MAP_OVERWRITE);
    if (ret < 0) {
        char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("[H264Encoder] av_hwframe_map(QSV→D3D11) failed: " + std::string(errbuf) +
                  " — encode_gpu_texture unavailable (QSV may not be derived from D3D11VA)");
        av_frame_free(&mapped);
        return ErrorCode::ENCODING_ERROR;
    }

    // mapped->data[0] = ID3D11Texture2D*（QSV 帧的 D3D11 后备纹理）
    // mapped->data[1] = (uint8_t*)(uintptr_t) array_index（纹理数组切片下标）
    auto* dst_tex = reinterpret_cast<ID3D11Texture2D*>(mapped->data[0]);
    auto  dst_idx = static_cast<UINT>(reinterpret_cast<uintptr_t>(mapped->data[1]));

    // ── 4. GPU Copy：快照 NV12 → QSV D3D11 纹理（Y + UV 两平面）──────────────
    // NV12 纹理数组 subresource 布局：Y[0..N-1], UV[N..2N-1]，N = ArraySize
    D3D11_TEXTURE2D_DESC dst_desc{};
    dst_tex->GetDesc(&dst_desc);
    const UINT uv_dst = dst_desc.ArraySize + dst_idx;

    auto& shared = SharedD3D11Device::instance();
    auto* ctx    = shared.context();
    // Y 平面
    ctx->CopySubresourceRegion(dst_tex, dst_idx, 0, 0, 0, nv12_texture, 0, nullptr);
    // UV 平面（NV12 快照 subresource 1 → QSV 纹理数组 UV subresource）
    ctx->CopySubresourceRegion(dst_tex, uv_dst,  0, 0, 0, nv12_texture, 1, nullptr);

    av_frame_unref(mapped);   // 解映射（必须在 CopySubresourceRegion 后）
    av_frame_free(&mapped);

    // ── 5. 设置 PTS 和关键帧标志 ─────────────────────────────────────────────
    hw_frame_->pts = pts_ms;
    if (force_keyframe_) {
        hw_frame_->pict_type = AV_PICTURE_TYPE_I;
        hw_frame_->key_frame = 1;
        force_keyframe_ = false;
        LOG_INFO("[H264Encoder] [GPU] Force IDR frame at pts=" + std::to_string(pts_ms));
    } else {
        hw_frame_->pict_type = AV_PICTURE_TYPE_NONE;
        hw_frame_->key_frame = 0;
    }

    // ── 6. 送入编码器 ─────────────────────────────────────────────────────────
    ret = avcodec_send_frame(codec_ctx_, hw_frame_);
    if (ret < 0) {
        char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("[H264Encoder] avcodec_send_frame (gpu texture) failed: " + std::string(errbuf));
        return ErrorCode::ENCODING_ERROR;
    }

    return receive_packets(packets);
}

ErrorCode H264Encoder::encode(const std::shared_ptr<VideoFrame>& frame, std::vector<EncodedPacketPtr>& packets) {
    if (!initialized_) {
        LOG_ERROR("H.264 encoder not initialized");
        return ErrorCode::INIT_FAILED;
    }

    if (!frame) {
        LOG_ERROR("Invalid video frame");
        return ErrorCode::INVALID_PARAM;
    }

    packets.clear();

    // 🔧 添加编码耗时诊断
    auto encode_start = std::chrono::high_resolution_clock::now();
    auto pts_start = frame->timestamp_ms;

    ErrorCode sret = send_frame_internal(frame);

    // 🔧 运行时故障检测：检查编码错误
    if (sret != ErrorCode::SUCCESS) {
        consecutive_errors_++;
        LOG_WARNING("[H264Encoder] Encoding error occurred (count: " + std::to_string(consecutive_errors_) + "/" +
                   std::to_string(MAX_CONSECUTIVE_ERRORS) + ")");

        // 连续错误达到阈值，切换到软件编码器
        if (consecutive_errors_ >= MAX_CONSECUTIVE_ERRORS && !has_exhausted_encoders_) {
            if (switch_to_next_encoder()) {
                // 切换成功后重试当前帧
                consecutive_errors_ = 0;
                sret = send_frame_internal(frame);

                if (sret == ErrorCode::SUCCESS) {
                    LOG_INFO("[H264Encoder] ✓ Retry with software encoder succeeded");
                }
            } else {
                // 切换失败，无法继续
                LOG_ERROR("[H264Encoder] Failed to switch to software encoder, cannot continue encoding");
                return ErrorCode::ENCODING_ERROR;
            }
        }
    } else {
        // 编码成功，重置错误计数器
        if (consecutive_errors_ > 0) {
            LOG_INFO("[H264Encoder] Encoding recovered after " + std::to_string(consecutive_errors_) + " errors");
            consecutive_errors_ = 0;
        }
    }

    if (sret != ErrorCode::SUCCESS) {
        return sret;
    }

    auto receive_start = std::chrono::high_resolution_clock::now();
    ErrorCode rret = receive_packets(packets);

    // 🔧 添加编码耗时诊断
    auto encode_end = std::chrono::high_resolution_clock::now();
    auto encode_duration = std::chrono::duration_cast<std::chrono::milliseconds>(encode_end - encode_start).count();
    auto receive_duration = std::chrono::duration_cast<std::chrono::milliseconds>(encode_end - receive_start).count();

    // 只在处理时间超过阈值或第一帧时记录日志
    static int frame_count = 0;
    frame_count++;
    if (encode_duration > 50 || frame_count <= 3 || frame_count % 100 == 0) {
        LOG_DEBUG("[H264Encoder] Encode #" + std::to_string(frame_count) +
                 " pts=" + std::to_string(pts_start) + "ms" +
                 " total=" + std::to_string(encode_duration) + "ms" +
                 " (send=" + std::to_string(receive_duration) + "ms" +
                 " recv=" + std::to_string(encode_duration - receive_duration) + "ms)" +
                 " packets=" + std::to_string(packets.size()));
    }

    return rret;
}

ErrorCode H264Encoder::flush(std::vector<EncodedPacketPtr>& packets) {
    if (!initialized_) {
        return ErrorCode::INIT_FAILED;
    }

    packets.clear();

    ErrorCode sret = send_flush();
    if (sret != ErrorCode::SUCCESS) {
        return sret;
    }

    return receive_packets(packets);
}

ErrorCode H264Encoder::set_bitrate(int bitrate) {
    if (!initialized_) {
        return ErrorCode::INIT_FAILED;
    }

    config_.bitrate = bitrate;
    if (codec_ctx_) {
        codec_ctx_->bit_rate = bitrate;
    }

    return ErrorCode::SUCCESS;
}

ErrorCode H264Encoder::force_keyframe() {
    if (!initialized_) {
        return ErrorCode::INIT_FAILED;
    }

    force_keyframe_ = true;
    LOG_INFO("Forcing keyframe in next frame");
    return ErrorCode::SUCCESS;
}

ErrorCode H264Encoder::reset() {
    if (!initialized_) {
        return ErrorCode::INVALID_STATE;
    }

    LOG_INFO("[H264Encoder] Resetting video encoder");

    // 重置状态标记
    force_keyframe_ = true;  // 重置后第一帧强制为关键帧
    first_keyframe_sent_ = false;
    total_frames_ = 0;

    // 刷新编码器缓冲区
    if (codec_ctx_) {
        avcodec_flush_buffers(codec_ctx_);
        LOG_DEBUG("[H264Encoder] Flushed codec buffers");
    }

    return ErrorCode::SUCCESS;
}

// 🔧 切换到下一个编码器（按优先级顺序）
bool H264Encoder::switch_to_next_encoder() {
    if (has_exhausted_encoders_) {
        return true;  // 已经尝试了所有编码器
    }

    // 尝试切换到下一个编码器
    int next_index = current_encoder_index_ + 1;
    if (next_index >= static_cast<int>(encoder_candidates_.size())) {
        LOG_ERROR("[H264Encoder] All encoders exhausted, no more fallback options");
        has_exhausted_encoders_ = true;
        return false;
    }

    const std::string& next_encoder_name = encoder_candidates_[next_index];
    LOG_ERROR("========================================");
    LOG_ERROR("[H264Encoder] Current encoder failed!");
    LOG_ERROR("[H264Encoder] Switching from '" + std::string(codec_->name) + "' to '" + next_encoder_name + "'");
    LOG_ERROR("[H264Encoder] Fallback progress: " + std::to_string(next_index + 1) +
              "/" + std::to_string(encoder_candidates_.size()));
    LOG_ERROR("========================================");

    // 保存当前配置
    VideoEncoderConfig old_config = config_;
    const AVCodec* old_codec = codec_;
    AVCodecContext* old_codec_ctx = codec_ctx_;
    AVFrame* old_frame = frame_;
    bool old_is_qsv = is_qsv_encoder_;

    // 查找下一个编码器
    const AVCodec* next_codec = avcodec_find_encoder_by_name(next_encoder_name.c_str());
    if (!next_codec) {
        LOG_ERROR("[H264Encoder] Failed to find encoder '" + next_encoder_name + "'");
        return false;
    }

    LOG_INFO("[H264Encoder] Found next encoder: " + next_encoder_name);

    // 检查是否为 QSV 编码器
    bool next_is_qsv = (next_encoder_name.find("qsv") != std::string::npos);

    // 创建新的编码器上下文
    AVCodecContext* new_codec_ctx = avcodec_alloc_context3(next_codec);
    if (!new_codec_ctx) {
        LOG_ERROR("[H264Encoder] Failed to alloc next codec context");
        return false;
    }

    // 配置新编码器
    new_codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    new_codec_ctx->codec_id = AV_CODEC_ID_H264;
    new_codec_ctx->width = old_config.width;
    new_codec_ctx->height = old_config.height;
    new_codec_ctx->time_base = get_time_base();
    new_codec_ctx->framerate = AVRational{old_config.fps > 0 ? old_config.fps : 30, 1};
    // ✅ GOP: 参考原项目，使用 1 秒关键帧间隔
    new_codec_ctx->gop_size = old_config.gop > 0 ? old_config.gop : old_config.fps;
    new_codec_ctx->keyint_min = old_config.fps / 2;
    new_codec_ctx->max_b_frames = 0;
    new_codec_ctx->bit_rate = old_config.bitrate;
    new_codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // 根据编码器类型设置像素格式和特殊配置
    if (next_is_qsv) {
        new_codec_ctx->pix_fmt = AV_PIX_FMT_QSV;
        // 创建 QSV 硬件设备和帧上下文（与初始化逻辑相同）
        AVBufferRef* new_hw_device_ctx = nullptr;
        int ret = av_hwdevice_ctx_create(&new_hw_device_ctx, AV_HWDEVICE_TYPE_QSV,
                                        "d3d11va", nullptr, 0);
        if (ret < 0) {
            ret = av_hwdevice_ctx_create(&new_hw_device_ctx, AV_HWDEVICE_TYPE_QSV,
                                        nullptr, nullptr, 0);
        }
        if (ret < 0) {
            LOG_ERROR("[H264Encoder] Failed to create QSV device context for fallback");
            avcodec_free_context(&new_codec_ctx);
            return false;
        }

        AVBufferRef* new_hw_frame_ctx = av_hwframe_ctx_alloc(new_hw_device_ctx);
        if (!new_hw_frame_ctx) {
            LOG_ERROR("[H264Encoder] Failed to allocate QSV hwframe context for fallback");
            av_buffer_unref(&new_hw_device_ctx);
            avcodec_free_context(&new_codec_ctx);
            return false;
        }

        AVHWFramesContext* frames_ctx = (AVHWFramesContext*)new_hw_frame_ctx->data;
        frames_ctx->format = AV_PIX_FMT_QSV;
        frames_ctx->sw_format = AV_PIX_FMT_NV12;
        frames_ctx->width = new_codec_ctx->width;
        frames_ctx->height = new_codec_ctx->height;
        frames_ctx->initial_pool_size = 20;

        ret = av_hwframe_ctx_init(new_hw_frame_ctx);
        if (ret < 0) {
            LOG_ERROR("[H264Encoder] Failed to init QSV hwframe context for fallback");
            av_buffer_unref(&new_hw_frame_ctx);
            av_buffer_unref(&new_hw_device_ctx);
            avcodec_free_context(&new_codec_ctx);
            return false;
        }

        new_codec_ctx->hw_frames_ctx = av_buffer_ref(new_hw_frame_ctx);
        av_buffer_unref(&new_hw_frame_ctx);
        av_buffer_unref(&new_hw_device_ctx);
        // QSV: 强制 avcC 格式
        if (new_codec_ctx->priv_data) {
            av_opt_set(new_codec_ctx->priv_data, "h264_demuxer", "avc", 0);
            LOG_INFO("[H264Encoder] Fallback QSV: set h264_demuxer=avc for avcC format");
        }
    } else if (next_encoder_name == "libx264") {
        new_codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        if (new_codec_ctx->priv_data) {
            av_opt_set(new_codec_ctx->priv_data, "preset", "ultrafast", 0);
            av_opt_set(new_codec_ctx->priv_data, "tune", "zerolatency", 0);
            std::string x264_params = "ref=1:slice-max-size=400:slices=4:profile=baseline:annexb=0:repeat-headers=0";
            av_opt_set(new_codec_ctx->priv_data, "x264-params", x264_params.c_str(), 0);
        }
    } else {
        // NVENC, AMF 等硬件编码器：强制 avcC 格式
        new_codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        if (new_codec_ctx->priv_data) {
            if (next_encoder_name.find("nvenc") != std::string::npos) {
                av_opt_set(new_codec_ctx->priv_data, "h264_demuxer", "avc", 0);
                av_opt_set(new_codec_ctx->priv_data, "delay", "0", 0);
                av_opt_set(new_codec_ctx->priv_data, "tune", "ll", 0);
                LOG_INFO("[H264Encoder] Fallback NVENC: set h264_demuxer=avc for avcC format");
            } else if (next_encoder_name.find("amf") != std::string::npos) {
                av_opt_set(new_codec_ctx->priv_data, "usage", "transcoding", 0);
                LOG_INFO("[H264Encoder] Fallback AMF: set usage=transcoding for avcC format");
            }
        }
    }

    // 尝试打开新编码器
    if (avcodec_open2(new_codec_ctx, next_codec, nullptr) < 0) {
        char errbuf[128];
        av_strerror(AVERROR_UNKNOWN, errbuf, sizeof(errbuf));
        LOG_ERROR("[H264Encoder] Failed to open encoder '" + next_encoder_name + "': " + errbuf);

        // 释放新分配的资源
        avcodec_free_context(&new_codec_ctx);

        // 释放旧编码器资源
        if (old_codec_ctx) {
            avcodec_free_context(&old_codec_ctx);
        }
        if (old_frame) {
            av_frame_free(&old_frame);
        }

        // 继续尝试下一个编码器
        current_encoder_index_ = next_index;
        return switch_to_next_encoder();
    }

    // 新编码器创建成功，现在可以安全释放旧编码器资源
    if (old_codec_ctx) {
        avcodec_free_context(&old_codec_ctx);
    }

    // 更新成员变量
    codec_ = next_codec;
    codec_ctx_ = new_codec_ctx;
    current_encoder_index_ = next_index;
    is_qsv_encoder_ = next_is_qsv;

    // 释放旧的 frame
    if (old_frame) {
        av_frame_free(&old_frame);
    }

    // 如果不是 QSV 编码器，创建新的软件 frame
    if (!next_is_qsv) {
        frame_ = av_frame_alloc();
        if (!frame_) {
            LOG_ERROR("[H264Encoder] Failed to alloc frame for new encoder");
            // 释放已打开的编码器
            avcodec_free_context(&codec_ctx_);
            codec_ = nullptr;
            codec_ctx_ = nullptr;
            return false;
        }
        frame_->format = codec_ctx_->pix_fmt;
        frame_->width = codec_ctx_->width;
        frame_->height = codec_ctx_->height;
        if (av_frame_get_buffer(frame_, 0) < 0) {
            LOG_ERROR("[H264Encoder] Failed to alloc frame buffer");
            av_frame_free(&frame_);
            frame_ = nullptr;
            // 释放已打开的编码器
            avcodec_free_context(&codec_ctx_);
            codec_ = nullptr;
            codec_ctx_ = nullptr;
            return false;
        }
    } else {
        frame_ = nullptr;  // QSV 编码器不需要预先分配 frame
    }

    // 释放旧的 sws context
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    sws_src_fmt_ = AV_PIX_FMT_NONE;
    sws_src_w_ = 0;
    sws_src_h_ = 0;

    consecutive_errors_ = 0;

    LOG_INFO("[H264Encoder] ✓ Successfully switched to encoder '" + next_encoder_name + "'");
    LOG_INFO("[H264Encoder] Encoding will continue with " + next_encoder_name);
    LOG_ERROR("========================================");

    return true;
}

} // namespace live_assistant
