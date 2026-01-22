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
    force_keyframe_ = true; // Force first frame to be keyframe for RTMP streaming
    // Prefer hardware encoder when requested and available.
    // We'll probe candidates in preferred order based on CPU vendor (Intel -> QSV first, AMD -> AMF first).
    codec_ = nullptr;
    if (config_.prefer_hw) {
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

        // Build candidates order
        if (!cpu_vendor.empty() && cpu_vendor.find("GenuineIntel") != std::string::npos) {
            candidates = {"h264_qsv", "h264_amf", "h264_nvenc"};
        } else if (!cpu_vendor.empty() && cpu_vendor.find("AuthenticAMD") != std::string::npos) {
            candidates = {"h264_amf", "h264_qsv", "h264_nvenc"};
        } else {
            candidates = {"h264_qsv", "h264_amf", "h264_nvenc"};
        }

        // Probe each candidate by trying to open a temporary codec context and checking codec parameters.
        for (const char* hw_name : candidates) {
            const AVCodec* probe_codec = avcodec_find_encoder_by_name(hw_name);
            if (!probe_codec) continue;

            // create temporary context for probing
            AVCodecContext* probe_ctx = avcodec_alloc_context3(probe_codec);
            if (!probe_ctx) continue;
            probe_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
            probe_ctx->width = config_.width > 0 ? config_.width : 1280;
            probe_ctx->height = config_.height > 0 ? config_.height : 720;
            probe_ctx->time_base = AVRational{1, config_.fps > 0 ? config_.fps : 30};
            probe_ctx->framerate = AVRational{config_.fps > 0 ? config_.fps : 30, 1};
            probe_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
            probe_ctx->bit_rate = config_.bitrate;
            probe_ctx->gop_size = config_.gop > 0 ? config_.gop : (config_.fps > 0 ? config_.fps * 2 : 60);
            probe_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            // If probing QSV, try creating a qsv hwdevice and attach it to the probe context
            AVBufferRef* hw_device_ctx = nullptr;
            if (std::string(hw_name).find("qsv") != std::string::npos) {
                if (av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_QSV, nullptr, nullptr, 0) == 0 && hw_device_ctx) {
                    // attach hwdevice to probe context
                    probe_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
                } else {
                    if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
                    hw_device_ctx = nullptr;
                }
            }

            int ret = avcodec_open2(probe_ctx, probe_codec, nullptr);
            if (ret >= 0) {
                AVCodecParameters* tmppar = avcodec_parameters_alloc();
                if (tmppar) {
                    if (avcodec_parameters_from_context(tmppar, probe_ctx) >= 0) {
                        // candidate works -> select it
                        codec_ = probe_codec;
                        avcodec_parameters_free(&tmppar);
                        avcodec_free_context(&probe_ctx);
                        if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
                        LOG_INFO(std::string("Selected hardware encoder after probe: ") + hw_name);
                        break;
                    }
                    avcodec_parameters_free(&tmppar);
                }
            }
            // cleanup and continue
            avcodec_free_context(&probe_ctx);
            if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
        }
        if (!codec_) {
            LOG_INFO("No suitable hardware encoder found after probing; will fallback to software x264");
        }
    }

    // Fallback to software encoder (libx264) if no hardware encoder was selected/found
    if (!codec_) {
        codec_ = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec_) {
            LOG_ERROR("Failed to find H264 encoder");
            return ErrorCode::INIT_FAILED;
        }
        LOG_INFO(std::string("Using software encoder: ") + codec_->name);
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
    codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    codec_ctx_->bit_rate = config_.bitrate;

    // For FLV/RTMP, extradata is typically required
    codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

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

    // Input pixel format may vary (RGBA/NV12). Initialize with RGBA; will be recreated on demand.
    sws_ctx_ = sws_getContext(
        config_.width,
        config_.height,
        AV_PIX_FMT_RGBA,
        config_.width,
        config_.height,
        codec_ctx_->pix_fmt,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr);

    if (!sws_ctx_) {
        LOG_ERROR("Failed to create sws context");
        return ErrorCode::INIT_FAILED;
    }

    initialized_ = true;

    LOG_INFO("H264 encoder initialized: " + std::to_string(config_.width) + "x" + std::to_string(config_.height) +
             " fps=" + std::to_string(config_.fps) + " bitrate=" + std::to_string(config_.bitrate));

    return ErrorCode::SUCCESS;
}

ErrorCode H264Encoder::shutdown() {
    initialized_ = false;

    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
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

    return ErrorCode::SUCCESS;
}

ErrorCode H264Encoder::send_frame_internal(const std::shared_ptr<VideoFrame>& in) {
    if (!in || !frame_ || !codec_ctx_) {
        return ErrorCode::INVALID_PARAM;
    }

    if (av_frame_make_writable(frame_) < 0) {
        return ErrorCode::ENCODING_ERROR;
    }

    AVPixelFormat src_fmt = AV_PIX_FMT_RGBA;
    if (in->format == VideoFrame::PixelFormat::NV12) {
        src_fmt = AV_PIX_FMT_NV12;
    }

    if (!sws_ctx_ || sws_src_fmt_ != src_fmt || sws_src_w_ != in->width || sws_src_h_ != in->height) {
        if (sws_ctx_) {
            sws_freeContext(sws_ctx_);
            sws_ctx_ = nullptr;
        }

        sws_ctx_ = sws_getContext(
            config_.width,
            config_.height,
            src_fmt,
            config_.width,
            config_.height,
            codec_ctx_->pix_fmt,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
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

        sws_scale(
            sws_ctx_,
            src_slices,
            src_stride,
            0,
            in->height,
            frame_->data,
            frame_->linesize);
    } else {
    const uint8_t* src_slices[1] = {reinterpret_cast<const uint8_t*>(in->data.get())};
    int src_stride[1] = {in->stride > 0 ? in->stride : in->width * 4};

    sws_scale(
        sws_ctx_,
        src_slices,
        src_stride,
        0,
        in->height,
        frame_->data,
        frame_->linesize);
    }

    // Use timestamp from VideoFrame for proper A/V sync
    // Convert microseconds to time_base units
    int64_t pts = av_rescale_q(in->timestamp.us, AV_TIME_BASE_Q, codec_ctx_->time_base);
    frame_->pts = pts;

    if (force_keyframe_) {
        frame_->pict_type = AV_PICTURE_TYPE_I;
        force_keyframe_ = false;
    } else {
        frame_->pict_type = AV_PICTURE_TYPE_NONE;
    }

    if (avcodec_send_frame(codec_ctx_, frame_) < 0) {
        LOG_ERROR("avcodec_send_frame failed");
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
    LOG_INFO("Forcing keyframe in next frame");
    return ErrorCode::SUCCESS;
}

} // namespace live_assistant
