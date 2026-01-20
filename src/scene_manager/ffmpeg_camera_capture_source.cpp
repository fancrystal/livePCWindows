#include "scene_manager/ffmpeg_camera_capture_source.h"
#include "common/log.h"

#include <chrono>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

namespace live_assistant {

FFmpegCameraCaptureSource::FFmpegCameraCaptureSource(const CaptureConfig& config)
    : config_(config) {
}

FFmpegCameraCaptureSource::~FFmpegCameraCaptureSource() {
    stop();
    shutdown();
}

bool FFmpegCameraCaptureSource::initialize() {
    if (config_.type != CaptureConfig::TargetType::CAMERA) {
        LOG_ERROR("FFmpegCameraCaptureSource: invalid config type");
        return false;
    }

    // target_id holds dshow device_name (e.g. @device_pnp_...)
    if (config_.target_id.empty()) {
        LOG_ERROR("FFmpegCameraCaptureSource: empty device_name");
        return false;
    }

    avdevice_register_all();

    const AVInputFormat* ifmt = av_find_input_format("dshow");
    if (!ifmt) {
        LOG_ERROR("FFmpegCameraCaptureSource: dshow input format not found");
        return false;
    }

    AVFormatContext* fmt = nullptr;

    std::string url = "video=" + config_.target_id;

    AVDictionary* options = nullptr;
    if (config_.fps > 0) {
        av_dict_set(&options, "framerate", std::to_string(config_.fps).c_str(), 0);
    }

    int ret = avformat_open_input(&fmt, url.c_str(), const_cast<AVInputFormat*>(ifmt), &options);
    av_dict_free(&options);

    if (ret < 0 || !fmt) {
        LOG_ERROR("FFmpegCameraCaptureSource: avformat_open_input failed for url: " + url);
        return false;
    }

    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        LOG_ERROR("FFmpegCameraCaptureSource: avformat_find_stream_info failed");
        avformat_close_input(&fmt);
        return false;
    }

    int vindex = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i] && fmt->streams[i]->codecpar && fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vindex = static_cast<int>(i);
            break;
        }
    }

    if (vindex < 0) {
        LOG_ERROR("FFmpegCameraCaptureSource: no video stream");
        avformat_close_input(&fmt);
        return false;
    }

    AVCodecParameters* par = fmt->streams[vindex]->codecpar;
    const AVCodec* dec = avcodec_find_decoder(par->codec_id);
    if (!dec) {
        LOG_ERROR("FFmpegCameraCaptureSource: decoder not found");
        avformat_close_input(&fmt);
        return false;
    }

    AVCodecContext* cc = avcodec_alloc_context3(dec);
    if (!cc) {
        avformat_close_input(&fmt);
        return false;
    }

    if (avcodec_parameters_to_context(cc, par) < 0) {
        avcodec_free_context(&cc);
        avformat_close_input(&fmt);
        return false;
    }

    if (avcodec_open2(cc, dec, nullptr) < 0) {
        LOG_ERROR("FFmpegCameraCaptureSource: avcodec_open2 failed");
        avcodec_free_context(&cc);
        avformat_close_input(&fmt);
        return false;
    }

    fmt_ctx_ = fmt;
    codec_ctx_ = cc;
    video_stream_index_ = vindex;

    LOG_INFO("FFmpegCameraCaptureSource initialized: " + config_.target_id);
    return true;
}

bool FFmpegCameraCaptureSource::start() {
    if (running_.load()) return true;
    if (!fmt_ctx_ || !codec_ctx_) {
        LOG_ERROR("FFmpegCameraCaptureSource: not initialized");
        return false;
    }

    stop_flag_ = false;
    running_ = true;
    th_ = std::thread(&FFmpegCameraCaptureSource::capture_loop, this);
    return true;
}

bool FFmpegCameraCaptureSource::stop() {
    if (!running_.load()) return true;
    stop_flag_ = true;
    if (th_.joinable()) th_.join();
    running_ = false;
    return true;
}

bool FFmpegCameraCaptureSource::shutdown() {
    if (sws_ctx_) {
        sws_freeContext(static_cast<SwsContext*>(sws_ctx_));
        sws_ctx_ = nullptr;
    }

    if (codec_ctx_) {
        AVCodecContext* cc = static_cast<AVCodecContext*>(codec_ctx_);
        avcodec_free_context(&cc);
        codec_ctx_ = nullptr;
    }

    if (fmt_ctx_) {
        AVFormatContext* fmt = static_cast<AVFormatContext*>(fmt_ctx_);
        avformat_close_input(&fmt);
        fmt_ctx_ = nullptr;
    }

    video_stream_index_ = -1;
    return true;
}

void FFmpegCameraCaptureSource::capture_loop() {
    LOG_INFO("FFmpegCameraCaptureSource capture_loop started for " + config_.target_id);
    auto* fmt = static_cast<AVFormatContext*>(fmt_ctx_);
    auto* cc = static_cast<AVCodecContext*>(codec_ctx_);
    if (!fmt || !cc) {
        LOG_ERROR("FFmpegCameraCaptureSource capture_loop: context is null");
        running_ = false;
        return;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* bgra = av_frame_alloc();

    if (!pkt || !frame || !bgra) {
        LOG_ERROR("FFmpegCameraCaptureSource capture_loop: failed to allocate ffmpeg objects");
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        if (bgra) av_frame_free(&bgra);
        running_ = false;
        return;
    }

    int src_w = 0, src_h = 0;
    AVPixelFormat src_fmt = AV_PIX_FMT_NONE;

    int64_t fail_count = 0;
    int64_t frame_count = 0;

    while (!stop_flag_.load()) {
        int r = av_read_frame(fmt, pkt);
        if (r < 0) {
            fail_count++;
            if (fail_count % 100 == 1) { // Log every 100 failures
                char err_buf[AV_ERROR_MAX_STRING_SIZE] = {0};
                av_make_error_string(err_buf, AV_ERROR_MAX_STRING_SIZE, r);
                LOG_WARNING("FFmpegCameraCaptureSource av_read_frame failed (count=" + std::to_string(fail_count) + "): " + err_buf);
            }
            av_packet_unref(pkt);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        fail_count = 0;

        if (pkt->stream_index != video_stream_index_) {
            av_packet_unref(pkt);
            continue;
        }

        if (avcodec_send_packet(cc, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);

        while (avcodec_receive_frame(cc, frame) == 0) {
            if (frame_count == 0) {
                LOG_INFO("FFmpegCameraCaptureSource: first frame decoded! size=" + std::to_string(frame->width) + "x" + std::to_string(frame->height) + " format=" + av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)));
            }

            if (src_w != frame->width || src_h != frame->height || src_fmt != static_cast<AVPixelFormat>(frame->format) || !sws_ctx_) {
                if (sws_ctx_) sws_freeContext(static_cast<SwsContext*>(sws_ctx_));
                src_w = frame->width;
                src_h = frame->height;
                src_fmt = static_cast<AVPixelFormat>(frame->format);

                sws_ctx_ = sws_getContext(src_w, src_h, src_fmt, src_w, src_h, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!sws_ctx_) {
                    LOG_ERROR("FFmpegCameraCaptureSource: sws_getContext failed");
                    continue;
                }

                bgra->format = AV_PIX_FMT_BGRA;
                bgra->width = src_w;
                bgra->height = src_h;
                if (av_frame_get_buffer(bgra, 32) < 0) {
                    LOG_ERROR("FFmpegCameraCaptureSource: av_frame_get_buffer for bgra frame failed");
                    sws_freeContext(static_cast<SwsContext*>(sws_ctx_));
                    sws_ctx_ = nullptr;
                    continue;
                }
            }

            sws_scale(static_cast<SwsContext*>(sws_ctx_), frame->data, frame->linesize, 0, src_h, bgra->data, bgra->linesize);

            QImage img(bgra->data[0], src_w, src_h, bgra->linesize[0], QImage::Format_ARGB32);
            QImage deep = img.copy();

            CaptureFrame out;
            out.image = std::move(deep);
            out.timestamp.us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            out.width = src_w;
            out.height = src_h;

            emit frameReady(out);

            frame_count++;
        }
    }

    av_frame_free(&bgra);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    LOG_INFO("FFmpegCameraCaptureSource capture_loop finished for " + config_.target_id);
}

} // namespace live_assistant
