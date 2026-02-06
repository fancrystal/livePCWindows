#include "encoder/video_encoder.h"
#include "common/log.h"
#include "common/error.h"
#include "video_engine/video_engine.h"  // For VideoFrame definition
#if defined(_MSC_VER)
#include <intrin.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/hwcontext.h>
}

namespace live_assistant {

H264Encoder::H264Encoder() {
    LOG_INFO("H264Encoder constructor");
}

H264Encoder::~H264Encoder() {
    shutdown();
    LOG_INFO("H264Encoder destructor");
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
    return AVRational{1, config_.fps > 0 ? config_.fps : 30};
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

        // Build candidates order based on CPU vendor (智能选择策略)
        // Intel CPU: 优先集显QSV → 独显NVENC → 软编libx264
        // AMD CPU: 优先集显AMF → 独显NVENC → 软编libx264
        // 其他: NVENC → QSV → AMF → 软编libx264
        if (!cpu_vendor.empty() && cpu_vendor.find("GenuineIntel") != std::string::npos) {
            candidates = {"h264_qsv", "h264_nvenc", "libx264"};
            LOG_INFO("[H264Encoder] Intel CPU detected → Priority: QSV(iGPU) → NVENC(dGPU) → Software");
        } else if (!cpu_vendor.empty() && cpu_vendor.find("AuthenticAMD") != std::string::npos) {
            candidates = {"h264_amf", "h264_nvenc", "libx264"};
            LOG_INFO("[H264Encoder] AMD CPU detected → Priority: AMF(iGPU) → NVENC(dGPU) → Software");
        } else {
            candidates = {"h264_nvenc", "h264_qsv", "h264_amf", "libx264"};
            LOG_INFO("[H264Encoder] Unknown CPU vendor → Priority: NVENC → QSV → AMF → Software");
        }

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
            // 注意：虽然QSV硬件加速通常要求NV12，但YUV420P也支持
            // YUV420P更稳定，优先使用
            probe_ctx->pix_fmt = AV_PIX_FMT_YUV420P;  // 统一使用 YUV420P

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
    // GOP: prefer explicit config.gop (frames). Default to 2 seconds worth of frames.
    codec_ctx_->gop_size = config_.gop > 0 ? config_.gop : (config_.fps > 0 ? config_.fps * 2 : 60);
    codec_ctx_->max_b_frames = config_.b_frames_enabled ? 2 : 0;

    // 🔧 像素格式设置
    if (is_qsv_encoder_) {
        // QSV 编码器需要使用 AV_PIX_FMT_QSV 格式
        // 实际的像素格式（NV12）在硬件帧上下文中指定
        codec_ctx_->pix_fmt = AV_PIX_FMT_QSV;
        LOG_INFO("[H264Encoder] Using QSV pixel format (hardware acceleration)");

        // ✅ 创建 QSV 硬件设备上下文
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
            LOG_INFO("[H264Encoder] QSV hardware device context created");
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

    codec_ctx_->bit_rate = config_.bitrate;

    // For FLV/RTMP, extradata is typically required
    codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // 🔧 NVENC 低延迟配置（必须在 avcodec_open2 之前设置）
    if (codec_ctx_->priv_data && codec_ &&
        std::string(codec_->name).find("nvenc") != std::string::npos) {
        // 强制低延迟模式，减少编码延迟
        av_opt_set(codec_ctx_->priv_data, "delay", "0", 0);
        // zerolatency 模式优化延迟
        av_opt_set(codec_ctx_->priv_data, "tune", "ll", 0);  // ll = low latency
        // 禁用B帧以减少延迟和不连续性（B帧会导致dts/pts不同步）
        codec_ctx_->max_b_frames = 0;
        LOG_INFO("H264Encoder: configured NVENC for low-latency streaming");
    }

    // x264 options (works when underlying encoder is libx264)
    if (codec_ctx_->priv_data) {
        av_opt_set(codec_ctx_->priv_data, "preset", preset_to_string(config_.preset).c_str(), 0);
        av_opt_set(codec_ctx_->priv_data, "tune", "zerolatency", 0);

        // Only set x264-specific params when using libx264 (software) encoder.
        if (codec_ && std::string(codec_->name).find("libx264") != std::string::npos) {
            // Base x264 params to control refs/slices/profile and avoid huge NAL units.
            // More aggressive slicing to avoid oversized NAL units that break FLV/RTMP
            std::string x264_params = "ref=1:slice-max-size=400:slices=8:profile=baseline";

            // Rate control: respect user-selected mode (VBR/CBR/CQP)
            if (config_.mode == VideoEncodingMode::VBR) {
                // VBR: set a max bitrate (vbv-maxrate) and buffer size to allow variability.
                x264_params += ":vbv-maxrate=" + std::to_string(config_.max_bitrate / 1000) +
                               ":vbv-bufsize=" + std::to_string((config_.max_bitrate / 1000) * 2);
            } else if (config_.mode == VideoEncodingMode::CBR) {
                // CBR: instruct x264 to use nal-hrd=cbr and tighten vbv limits
                x264_params += ":nal-hrd=cbr:vbv-maxrate=" + std::to_string(config_.max_bitrate / 1000) +
                               ":vbv-bufsize=" + std::to_string((config_.max_bitrate / 1000));
            } else if (config_.mode == VideoEncodingMode::CQP) {
                // CQP: map quality value to crf if provided; use quality as CRF for x264
                x264_params += ":crf=" + std::to_string(static_cast<int>(config_.quality));
            }

            av_opt_set(codec_ctx_->priv_data, "x264-params", x264_params.c_str(), 0);
            LOG_INFO(std::string("H264Encoder: set x264-params: ") + x264_params);
        } else {
            LOG_INFO("H264Encoder: codec not libx264, skipping x264-params configuration");
        }
    }

    if (avcodec_open2(codec_ctx_, codec_, nullptr) < 0) {
        LOG_ERROR("Failed to open H264 encoder");
        return ErrorCode::INIT_FAILED;
    }
    // Ensure the encoder will produce a keyframe for the first frame after initialization.
    force_keyframe_ = true;

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
    is_qsv_encoder_ = false;

    return ErrorCode::SUCCESS;
}

ErrorCode H264Encoder::send_frame_internal(const std::shared_ptr<VideoFrame>& in) {
    if (!in || !codec_ctx_) {
        return ErrorCode::INVALID_PARAM;
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

        // 将输入数据转换为 NV12（软件帧）
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

        // 转换输入数据到软件帧（NV12）
        if (in->format == VideoFrame::PixelFormat::NV12) {
            const uint8_t* src_slices[2] = {in->data.get(), in->data_uv.get()};
            int src_stride[2] = {in->stride, in->stride_uv};
            sws_scale(sws_ctx_, src_slices, src_stride, 0, in->height,
                     sw_frame_->data, sw_frame_->linesize);
        } else {
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

    // Use timestamp from VideoFrame for proper A/V sync
    // Convert microseconds to time_base units
    // 🔧 确保第一帧从 0 开始，避免播放器等待同步
    if (first_frame_timestamp_us_ == -1) {
        first_frame_timestamp_us_ = in->timestamp.us;
        LOG_INFO("[H264Encoder] First video frame timestamp: " + std::to_string(in->timestamp.us / 1000) + "ms");
    }
    int64_t relative_timestamp_us = in->timestamp.us - first_frame_timestamp_us_;
    int64_t pts = av_rescale_q(relative_timestamp_us, AV_TIME_BASE_Q, codec_ctx_->time_base);
    encode_frame->pts = pts;

    if (force_keyframe_) {
        encode_frame->pict_type = AV_PICTURE_TYPE_I;
        force_keyframe_ = false;
    } else {
        encode_frame->pict_type = AV_PICTURE_TYPE_NONE;
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

        auto out = std::make_shared<EncodedPacket>();
        out->type = MediaType::VIDEO;
        out->pts = pkt->pts;
        out->dts = pkt->dts;
        out->duration = pkt->duration;
        out->encoder_time_base = get_time_base();
        out->is_keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        out->priority = 1;
        out->pkt = AVPacketPtr(pkt);

        packets.push_back(std::move(out));
    }

    return ErrorCode::SUCCESS;
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

    return receive_packets(packets);
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
    first_frame_timestamp_us_ = -1;  // 重置第一帧时间戳基准
    LOG_INFO("Forcing keyframe in next frame, reset first frame timestamp");
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
    new_codec_ctx->gop_size = old_config.gop > 0 ? old_config.gop : (old_config.fps > 0 ? old_config.fps * 2 : 60);
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
    } else if (next_encoder_name == "libx264") {
        new_codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        if (new_codec_ctx->priv_data) {
            av_opt_set(new_codec_ctx->priv_data, "preset", "ultrafast", 0);
            av_opt_set(new_codec_ctx->priv_data, "tune", "zerolatency", 0);
            std::string x264_params = "ref=1:slice-max-size=400:slices=4:profile=baseline";
            av_opt_set(new_codec_ctx->priv_data, "x264-params", x264_params.c_str(), 0);
        }
    } else {
        // NVENC, AMF 等硬件编码器
        new_codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    }

    // 尝试打开新编码器
    if (avcodec_open2(new_codec_ctx, next_codec, nullptr) < 0) {
        char errbuf[128];
        av_strerror(AVERROR_UNKNOWN, errbuf, sizeof(errbuf));
        LOG_ERROR("[H264Encoder] Failed to open encoder '" + next_encoder_name + "': " + errbuf);
        avcodec_free_context(&new_codec_ctx);
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
            return false;
        }
        frame_->format = codec_ctx_->pix_fmt;
        frame_->width = codec_ctx_->width;
        frame_->height = codec_ctx_->height;
        if (av_frame_get_buffer(frame_, 0) < 0) {
            LOG_ERROR("[H264Encoder] Failed to alloc frame buffer");
            av_frame_free(&frame_);
            frame_ = nullptr;
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
