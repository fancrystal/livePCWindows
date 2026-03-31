#include "stream_pusher/rtmp_pusher.h"
#include "common/log.h"
#include "common/error.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
}

#include <cstdio>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <vector>
#include <cstring>

#ifdef _WIN32
#define NOMINMAX   // 防止 windows.h 的 min/max 宏污染 std::min/std::max
#include <windows.h>
#endif

namespace live_assistant {

// ─── Annex-B → avcC extradata 转换 ──────────────────────────────────────────
// NVENC 等编码器即使设置了 AV_CODEC_FLAG_GLOBAL_HEADER，extradata 仍可能是
// Annex-B（00 00 00 01 起始码）格式。FLV/RTMP 要求 avcC 格式（ISO 14496-15），
// 否则 CDN 无法解析 SPS/PPS，导致花屏或流中断。
static bool convert_annexb_to_avcc(AVCodecParameters* par)
{
    if (!par || par->extradata_size < 5) return false;
    const uint8_t* d = par->extradata;
    // 检测是否为 Annex-B（以 00 00 00 01 或 00 00 01 开头）
    bool is_annexb = (d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 1) ||
                     (d[0] == 0 && d[1] == 0 && d[2] == 1);
    if (!is_annexb) return false;  // 已经是 avcC，无需转换

    // 扫描所有 NAL unit，提取第一个 SPS(7) 和第一个 PPS(8)
    std::vector<uint8_t> sps, pps;
    int size = par->extradata_size;
    int i = 0;
    while (i < size) {
        // 跳过起始码
        int sc_len = 0;
        if (i + 3 < size && d[i]==0 && d[i+1]==0 && d[i+2]==0 && d[i+3]==1) sc_len = 4;
        else if (i + 2 < size && d[i]==0 && d[i+1]==0 && d[i+2]==1) sc_len = 3;
        else { i++; continue; }

        int nal_start = i + sc_len;
        if (nal_start >= size) break;

        // 找下一个起始码（NAL 的结尾）
        int nal_end = size;
        for (int j = nal_start + 1; j < size - 2; j++) {
            if (d[j]==0 && d[j+1]==0 && (d[j+2]==1 ||
                (j+3 < size && d[j+2]==0 && d[j+3]==1))) {
                nal_end = j;
                break;
            }
        }

        int nal_type = d[nal_start] & 0x1F;
        if (nal_type == 7 && sps.empty())
            sps.assign(d + nal_start, d + nal_end);
        else if (nal_type == 8 && pps.empty())
            pps.assign(d + nal_start, d + nal_end);

        i = nal_end;
        if (!sps.empty() && !pps.empty()) break;
    }

    if (sps.size() < 4 || pps.empty()) return false;

    // 组装 avcC（ISO 14496-15 Section 5.2.4.1）
    std::vector<uint8_t> avcc;
    avcc.reserve(11 + sps.size() + pps.size());
    avcc.push_back(1);                              // configurationVersion
    avcc.push_back(sps[1]);                         // AVCProfileIndication
    avcc.push_back(sps[2]);                         // profile_compatibility
    avcc.push_back(sps[3]);                         // AVCLevelIndication
    avcc.push_back(0xFF);                           // lengthSizeMinusOne = 3 → 4字节长度前缀
    avcc.push_back(0xE1);                           // numSPS = 1（高3位为保留位）
    avcc.push_back(static_cast<uint8_t>(sps.size() >> 8));
    avcc.push_back(static_cast<uint8_t>(sps.size() & 0xFF));
    avcc.insert(avcc.end(), sps.begin(), sps.end());
    avcc.push_back(1);                              // numPPS = 1
    avcc.push_back(static_cast<uint8_t>(pps.size() >> 8));
    avcc.push_back(static_cast<uint8_t>(pps.size() & 0xFF));
    avcc.insert(avcc.end(), pps.begin(), pps.end());

    // 替换 codecpar->extradata
    av_free(par->extradata);
    par->extradata = static_cast<uint8_t*>(av_malloc(avcc.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!par->extradata) { par->extradata_size = 0; return false; }
    memcpy(par->extradata, avcc.data(), avcc.size());
    memset(par->extradata + avcc.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
    par->extradata_size = static_cast<int>(avcc.size());
    return true;
}

// ─── SEH 保护 wrapper ────────────────────────────────────────────────────────
// av_interleaved_write_frame / av_write_frame 在服务端突然关闭 TCP 连接时，
// 可能在 FFmpeg 内部触发 Access Violation（SEH）而不是返回错误码，
// 从而直接崩溃整个进程。
//
// 使用单独的 free function（无 C++ 析构对象）包裹 write_frame，
// 用 __try/__except 捕获 SEH 并转换为 AVERROR_UNKNOWN 返回值，
// 使上层可以走正常的 free_resources + 重连路径，而不是进程崩溃。
// ─────────────────────────────────────────────────────────────────────────────
static int safe_write_frame(AVFormatContext* fmt_ctx, AVPacket* pkt, bool interleaved)
{
#ifdef _WIN32
    __try {
        int ret = interleaved
            ? av_interleaved_write_frame(fmt_ctx, pkt)
            : av_write_frame(fmt_ctx, pkt);
        return ret;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // SEH 捕获：FFmpeg 内部 Access Violation 等硬件异常
        // 返回 AVERROR_UNKNOWN，让上层 free_resources() 并触发重连
        return AVERROR_UNKNOWN;
    }
#else
    return interleaved
        ? av_interleaved_write_frame(fmt_ctx, pkt)
        : av_write_frame(fmt_ctx, pkt);
#endif
}

RTMPPusher::RTMPPusher() {
    avformat_network_init();
    LOG_INFO("RTMPPusher constructor");
}

RTMPPusher::~RTMPPusher() {
    disconnect();
    free_resources();
    if (cached_audio_codecpar_) {
        avcodec_parameters_free(&cached_audio_codecpar_);
    }
    if (cached_video_codecpar_) {
        avcodec_parameters_free(&cached_video_codecpar_);
    }
    avformat_network_deinit();
    LOG_INFO("RTMPPusher destructor");
}

ErrorCode RTMPPusher::initialize(const StreamConfig& config) {
    LOG_INFO("Initializing RTMP pusher");
    
    // If already initialized (format_ctx_ exists), skip re-initialization to avoid
    // clearing previously registered streams.
    if (format_ctx_) {
        LOG_INFO("RTMP pusher already initialized, skipping re-initialize");
        config_ = config;
        return ErrorCode::SUCCESS;
    }

    config_ = config;
    header_written_ = false;
    have_sent_first_key_ = false;  // 重置第一关键帧标志，确保新推流从第一帧开始正确处理

    ErrorCode result = init_format_context();
    if (result != ErrorCode::SUCCESS) {
        LOG_ERROR("Failed to initialize format context");
        return result;
    }
    
    LOG_INFO("RTMP pusher initialized successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode RTMPPusher::init_format_context() {
    free_resources();

    if (avformat_alloc_output_context2(&format_ctx_, nullptr, "flv", nullptr) < 0) {
        LOG_ERROR("Failed to allocate output context");
        return ErrorCode::INIT_FAILED;
    }
    
    if (config_.low_latency && format_ctx_->priv_data) {
        av_opt_set(format_ctx_->priv_data, "rtmp_live", "live", 0);
        av_opt_set(format_ctx_->priv_data, "rtmp_buffer", "0", 0);
    }
    
    return ErrorCode::SUCCESS;
}

bool RTMPPusher::is_initialized() const {
    return format_ctx_ != nullptr;
}

ErrorCode RTMPPusher::register_audio_stream(AVCodecParameters* codecpar, AVRational time_base) {
    if (!format_ctx_) {
        return ErrorCode::INIT_FAILED;
    }
    if (header_written_) {
        LOG_ERROR("Cannot register stream after header written");
        return ErrorCode::INVALID_STATE;
    }

    // 缓存参数，供断线重连时自动重注册
    if (cached_audio_codecpar_) {
        avcodec_parameters_free(&cached_audio_codecpar_);
    }
    cached_audio_codecpar_ = avcodec_parameters_alloc();
    if (cached_audio_codecpar_) {
        avcodec_parameters_copy(cached_audio_codecpar_, codecpar);
    }
    cached_audio_time_base_ = time_base;

    audio_stream_ = avformat_new_stream(format_ctx_, nullptr);
    if (!audio_stream_) {
        LOG_ERROR("Failed to create audio stream");
        return ErrorCode::INIT_FAILED;
    }

    if (avcodec_parameters_copy(audio_stream_->codecpar, codecpar) < 0) {
        LOG_ERROR("Failed to copy audio codec parameters");
        return ErrorCode::INIT_FAILED;
    }

    // Log audio codec parameters for debugging
    LOG_INFO("[RTMP] Registered audio stream:");
    LOG_INFO("  - codec_id: " + std::to_string(audio_stream_->codecpar->codec_id));
    //LOG_INFO("  - channels: " + std::to_string(audio_stream_->codecpar->channels));
    LOG_INFO("  - sample_rate: " + std::to_string(audio_stream_->codecpar->sample_rate));
    LOG_INFO("  - bit_rate: " + std::to_string(audio_stream_->codecpar->bit_rate));
    LOG_INFO("  - extradata_size: " + std::to_string(audio_stream_->codecpar->extradata_size));
    if (audio_stream_->codecpar->extradata_size > 0) {
        LOG_INFO("  - extradata[0]: 0x" + std::to_string(static_cast<int>(audio_stream_->codecpar->extradata[0])));
    }
    LOG_INFO("  - time_base: " + std::to_string(time_base.num) + "/" + std::to_string(time_base.den));

    audio_stream_->time_base = time_base;
    return ErrorCode::SUCCESS;
}

ErrorCode RTMPPusher::register_video_stream(AVCodecParameters* codecpar, AVRational time_base) {
    if (!format_ctx_) {
        return ErrorCode::INIT_FAILED;
    }
    if (header_written_) {
        LOG_ERROR("Cannot register stream after header written");
        return ErrorCode::INVALID_STATE;
    }

    // 缓存参数，供断线重连时自动重注册
    if (cached_video_codecpar_) {
        avcodec_parameters_free(&cached_video_codecpar_);
    }
    cached_video_codecpar_ = avcodec_parameters_alloc();
    if (cached_video_codecpar_) {
        avcodec_parameters_copy(cached_video_codecpar_, codecpar);
        // 确保缓存的 extradata 也是 avcC 格式（断线重连时直接使用）
        convert_annexb_to_avcc(cached_video_codecpar_);
    }
    cached_video_time_base_ = time_base;

    video_stream_ = avformat_new_stream(format_ctx_, nullptr);
    if (!video_stream_) {
        LOG_ERROR("Failed to create video stream");
        return ErrorCode::INIT_FAILED;
    }

    if (avcodec_parameters_copy(video_stream_->codecpar, codecpar) < 0) {
        LOG_ERROR("Failed to copy video codec parameters");
        return ErrorCode::INIT_FAILED;
    }

    // FLV/RTMP 要求 H264 extradata 必须是 avcC 格式（ISO 14496-15）。
    // NVENC 某些 Windows FFmpeg 版本即便设置了 AV_CODEC_FLAG_GLOBAL_HEADER 仍输出
    // Annex-B 格式（以 00 00 00 01 起始），需要手动转换。
    if (convert_annexb_to_avcc(video_stream_->codecpar)) {
        LOG_INFO("[RTMP] Converted video extradata from Annex-B to avcC format");
    }

    // Validate/log H264 codecpar and extradata to ensure avcC is propagated.
    LOG_INFO("[RTMP] Registered video stream:");
    LOG_INFO("  - codec_id: " + std::to_string(video_stream_->codecpar->codec_id));
    LOG_INFO("  - width: " + std::to_string(video_stream_->codecpar->width));
    LOG_INFO("  - height: " + std::to_string(video_stream_->codecpar->height));
    LOG_INFO("  - bit_rate: " + std::to_string(video_stream_->codecpar->bit_rate));
    LOG_INFO("  - extradata_size: " + std::to_string(video_stream_->codecpar->extradata_size));
    if (video_stream_->codecpar->extradata && video_stream_->codecpar->extradata_size > 0) {
        std::ostringstream oss;
        const int n = std::min(video_stream_->codecpar->extradata_size, 8);
        for (int i = 0; i < n; ++i) {
            if (i) oss << " ";
            oss << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(video_stream_->codecpar->extradata[i]);
        }
        LOG_INFO("  - extradata[0..7]: " + oss.str());

        if (video_stream_->codecpar->codec_id == AV_CODEC_ID_H264 &&
            (video_stream_->codecpar->extradata_size < 7 ||
             video_stream_->codecpar->extradata[0] != 1)) {
            LOG_WARNING("[RTMP] H264 extradata does not look like avcC (expected first byte 0x01)");
        }
    } else if (video_stream_->codecpar->codec_id == AV_CODEC_ID_H264) {
        LOG_WARNING("[RTMP] H264 stream has no extradata; muxers/players may fail to decode");
    }
    LOG_INFO("  - time_base: " + std::to_string(time_base.num) + "/" + std::to_string(time_base.den));

    video_stream_->time_base = time_base;
    return ErrorCode::SUCCESS;
}

ErrorCode RTMPPusher::open_output() {
    if (!format_ctx_) {
        return ErrorCode::INIT_FAILED;
    }

    std::string full_url = config_.server_url + "/" + config_.stream_key;
    // 调试模式：可选择输出到本地文件进行测试
    //// 要测试本地文件，请取消下面一行的注释：
    //full_url = "D:/test.flv";
    //// 正常推流时，请确保这一行被注释掉

    //// 对于本地文件测试，先删除已存在的文件以确保干净输出
    //// RTMP 推流时服务器会自动处理
    //if (full_url.find(".flv") != std::string::npos) {
    //    std::remove(full_url.c_str());  // 删除已存在的文件
    //}

    int ret = avio_open(&format_ctx_->pb, full_url.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR(std::string("Failed to open output URL: ") + full_url + ", error: " + errbuf);
        return ErrorCode::CONNECT_FAILED;
    }

    LOG_INFO(std::string("RTMP output opened: ") + full_url);
    connected_ = true;
    stats_.connected = true;
    stats_.reconnect_attempts++;

    return ErrorCode::SUCCESS;
}

ErrorCode RTMPPusher::connect_and_write_header() {
    LOG_INFO("[RTMP] connect_and_write_header() called: connected=" + std::to_string(connected_) +
             ", header_written=" + std::to_string(header_written_) +
             ", audio_stream=" + (audio_stream_ ? "valid" : "null") +
             ", video_stream=" + (video_stream_ ? "valid" : "null") +
             ", format_ctx=" + (format_ctx_ ? "valid" : "null") +
             ", cached_audio=" + (cached_audio_codecpar_ ? "valid" : "null") +
             ", cached_video=" + (cached_video_codecpar_ ? "valid" : "null"));

    if (connected_ && header_written_) {
        LOG_INFO("[RTMP] connect_and_write_header(): already connected, skipping");
        return ErrorCode::SUCCESS;
    }

    // 重连场景：format_ctx_ 被 free_resources() 清空，需要重建并重注册流
    if (!audio_stream_ || !video_stream_) {
        LOG_INFO("[RTMP] Streams missing, attempting re_register_cached_streams()");
        ErrorCode re_result = re_register_cached_streams();
        if (re_result != ErrorCode::SUCCESS) {
            LOG_ERROR("[RTMP] re_register_cached_streams() failed: " + std::to_string(static_cast<int>(re_result)));
            LOG_ERROR("Audio/video streams not registered and no cached params for reconnect");
            return ErrorCode::INVALID_STATE;
        }
        LOG_INFO("[RTMP] re_register_cached_streams() succeeded");
    }

    ErrorCode result = open_output();
    if (result != ErrorCode::SUCCESS) {
        return result;
    }

    if (avformat_write_header(format_ctx_, nullptr) < 0) {
        LOG_ERROR("Failed to write header");
        disconnect();
        return ErrorCode::INIT_FAILED;
    }

    // Override st->time_base after avformat_write_header.
    // avformat_write_header may set stream time_bases based on codecpar->time_base,
    // which can be {0,0} for uninitialized codecpar, resulting in st->time_base = {0,1}
    // and causing av_packet_rescale_ts to divide by zero → AV_NOPTS_VALUE.
    // We explicitly reset both streams to the intended {1,1000} (millisecond) time base.
    if (audio_stream_) {
        audio_stream_->time_base = {1, 1000};
        LOG_INFO("[RTMP] Audio stream time_base reset to 1/1000 after write_header");
    }
    if (video_stream_) {
        video_stream_->time_base = {1, 1000};
        LOG_INFO("[RTMP] Video stream time_base reset to 1/1000 after write_header");
    }

    header_written_ = true;
    have_sent_first_key_ = false;
    send_frame_count_ = 0;
    av_sync_offset_ms_ = 0;
    video_pts_base_ = -1;  // 每次重连重置，重新校准 QSV 内部 PTS 偏移
    audio_packet_count_ = 0;
    write_frame_count_ = 0;
    first_video_wait_initialized_ = false;

    // 初始化滑动窗口统计
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        auto now = std::chrono::steady_clock::now();
        window_start_time_ = now;
        video_packets_in_window_ = 0;
        audio_packets_in_window_ = 0;
        bytes_in_window_ = 0;
        last_calculated_fps_ = 0.0;
        last_calculated_bitrate_ = 0.0;
        stats_.start_time = now;
        stats_.bytes_sent = 0;
        stats_.video_packets_sent = 0;
        stats_.audio_packets_sent = 0;
    }

    LOG_INFO("Connected and wrote RTMP header");
    return ErrorCode::SUCCESS;
}

ErrorCode RTMPPusher::disconnect() {
    if (!connected_) {
        return ErrorCode::SUCCESS;
    }

    LOG_INFO("Disconnecting from RTMP server");

    if (format_ctx_ && header_written_) {
        av_write_trailer(format_ctx_);
    }

    if (format_ctx_ && format_ctx_->pb) {
        avio_close(format_ctx_->pb);
        format_ctx_->pb = nullptr;
    }

    connected_ = false;
    header_written_ = false;
    stats_.connected = false;

    // Free the format context to allow proper re-initialization
    free_resources();

    LOG_INFO("Disconnected from RTMP server");
    return ErrorCode::SUCCESS;
}

ErrorCode RTMPPusher::send_packet(const EncodedPacketPtr& packet) {
    if (!packet) {
        return ErrorCode::INVALID_PARAM;
    }

    if (!connected_ || !header_written_ || !format_ctx_ || !format_ctx_->pb) {
        return ErrorCode::NOT_CONNECTED;
    }

    if (!packet->pkt) {
        LOG_ERROR("RTMPPusher::send_packet - CRITICAL: packet->pkt is null!");
        return ErrorCode::INVALID_PARAM;
    }

    AVPacket* avpkt = packet->pkt.get();

    // 调试日志：打印每一帧的 PTS（仅首帧和每50帧）
    send_frame_count_++;
    if (packet->type == MediaType::AUDIO) {
        if (send_frame_count_ <= 3) {
            LOG_DEBUG("[RTMP] AUDIO frame #" + std::to_string(send_frame_count_) +
                     ": pts=" + std::to_string(packet->pts) + "ms" +
                     ", dts=" + std::to_string(packet->dts) + "ms" +
                     ", duration=" + std::to_string(packet->duration) + "ms");
        }
    } else {
        if (send_frame_count_ <= 3 || send_frame_count_ % 50 == 0) {
            LOG_DEBUG("[RTMP] VIDEO frame #" + std::to_string(send_frame_count_) +
                     ": pts=" + std::to_string(packet->pts) + "ms" +
                     ", dts=" + std::to_string(packet->dts) + "ms" +
                     ", duration=" + std::to_string(packet->duration) + "ms" +
                     ", is_keyframe=" + std::to_string(packet->is_keyframe ? 1 : 0));
        }
    }

    // 诊断第一帧 PTS（每次重连后重新记录，仅用于日志）
    if (packet->type == MediaType::AUDIO && av_sync_offset_ms_ == 0) {
        LOG_DEBUG("[RTMP] First AUDIO packet (pre-drop check): pts=" + std::to_string(packet->pts) +
                 ", wallclock=" + std::to_string(packet->wallclock_us / 1000) + "ms");
    }

    // CRITICAL: Log the packet state BEFORE any operation
    LOG_DEBUG("[RTMP] send_packet " + std::string(packet->type == MediaType::AUDIO ? "AUDIO" : "VIDEO") + 
              ": pkt->pts=" + std::to_string(avpkt->pts) +
              ", pkt->size=" + std::to_string(avpkt->size) +
              ", pkt->data[0]=" + std::to_string(avpkt->data ? avpkt->data[0] : -1) +
              ", pkt->stream_index=" + std::to_string(avpkt->stream_index));

    // Basic sanity checks for packet data
    if (!avpkt->data || avpkt->size <= 0) {
        LOG_WARNING("RTMPPusher::send_packet - invalid packet data (data=null or size<=0), dropping packet");
        return ErrorCode::INVALID_PARAM;
    }

    // CRITICAL: Clone the packet IMMEDIATELY to avoid race conditions with other threads
    // The encoder may reuse or modify the original packet while we're trying to send it
    AVPacket* cloned_pkt = av_packet_clone(avpkt);
    if (!cloned_pkt) {
        LOG_ERROR("RTMPPusher::send_packet - CRITICAL: failed to clone packet, cannot send");
        return ErrorCode::SEND_FAILED;
    }
    
    LOG_DEBUG("[RTMP] Clone SUCCESS: orig_size=" + std::to_string(avpkt->size) +
              ", cloned_size=" + std::to_string(cloned_pkt->size) +
              ", orig_pts=" + std::to_string(avpkt->pts) +
              ", cloned_pts=" + std::to_string(cloned_pkt->pts));
    
    AVPacket* write_pkt = cloned_pkt;
    AVPacket* pkt_to_free = cloned_pkt;

    // Now use the cloned packet for all operations
    avpkt = cloned_pkt;

    // 在第一帧视频 IDR 到来前丢弃所有音频包。
    // 防止音频超前于视频：若音频先于视频 IDR 进入 RTMP 流，音频会积累若干秒内容
    // （IDR 等待期间），而视频 IDR 到来后双方同归 0ms，音频已超前视频 N ms
    // （N = IDR 等待时长），播放器 A/V 同步时表现为画面滞后声音约 N ms（1-2 秒）。
    if (packet->type == MediaType::AUDIO && !have_sent_first_key_) {
        av_packet_free(&pkt_to_free);
        return ErrorCode::SUCCESS;
    }

    // Determine target stream and its time_base
    AVStream* st = nullptr;
    if (packet->type == MediaType::AUDIO) {
        if (!audio_stream_) {
            return ErrorCode::INVALID_STATE;
        }
        st = audio_stream_;
        
        // Log audio packet details for debugging (every 50 packets)
    audio_packet_count_++;
    if (audio_packet_count_ % 50 == 0) {
        LOG_DEBUG("[RTMP] Audio packet #" + std::to_string(audio_packet_count_) +
                 ": pts=" + std::to_string(packet->pts) +
                 ", duration=" + std::to_string(packet->duration) +
                 ", size=" + std::to_string(packet->pkt ? packet->pkt->size : 0));
    }
        
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.audio_packets_sent++;
            audio_packets_in_window_++;
        }
    } else {
        if (!video_stream_) {
            return ErrorCode::INVALID_STATE;
        }
        st = video_stream_;
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.video_packets_sent++;
            video_packets_in_window_++;
        }

        // If caller marked packet as keyframe, respect it
        if (packet->is_keyframe) {
            avpkt->flags |= AV_PKT_FLAG_KEY;
        }

        // 确保推流的第一帧视频是 IDR 关键帧。
        // 注意：不能设超时接受非关键帧——预览阶段 QSV 编码器已运行很长时间，
        // 推流开始时 clear_queue() 与编码线程存在竞态，可能有 PTS 极大的陈旧包残留。
        // 若以该陈旧包设置 video_pts_base_（如 14493ms），所有新帧 pts 均被 clamp 到 0，
        // 导致视频 PTS 比音频 PTS 快 ~14s，即音频慢 7-8 秒的音画不同步问题。
        if (packet->type == MediaType::VIDEO && !have_sent_first_key_) {
            if (!(avpkt->flags & AV_PKT_FLAG_KEY)) {
                // 初始化等待计时器（仅用于日志）
                if (!first_video_wait_initialized_) {
                    first_video_wait_start_ = std::chrono::steady_clock::now();
                    first_video_wait_initialized_ = true;
                }
                auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - first_video_wait_start_).count();
                LOG_WARNING("[RTMP] Dropping non-key video packet, waiting for IDR keyframe (" +
                           std::to_string(elapsed_ms) + "ms elapsed)");
                // 释放克隆包，避免内存泄漏
                av_packet_free(&pkt_to_free);
                return ErrorCode::SUCCESS;
            } else {
                LOG_INFO("[RTMP] First IDR keyframe received, size=" + std::to_string(avpkt->size));
                have_sent_first_key_ = true;
                first_video_wait_initialized_ = false;
            }
        }
    }

    avpkt->stream_index = st->index;

    // Debug: Log the time_base we're working with
    std::string media_type_str = (packet->type == MediaType::AUDIO) ? "AUDIO" : "VIDEO";
    LOG_DEBUG("[RTMP] send_packet: type=" + media_type_str +
              ", pts=" + std::to_string(packet->pts) +
              ", encoder_tb=" + std::to_string(packet->encoder_time_base.num) + "/" + std::to_string(packet->encoder_time_base.den) +
              ", stream_tb=" + std::to_string(st->time_base.num) + "/" + std::to_string(st->time_base.den));

    // Set packet timestamps in encoder time_base then rescale into stream time_base
    // Note: We're modifying the cloned packet, not the original
    avpkt->pts = packet->pts;
    avpkt->dts = packet->dts;
    avpkt->duration = packet->duration;

    if (packet->encoder_time_base.num > 0 && packet->encoder_time_base.den > 0) {
        int64_t old_pts = avpkt->pts;
        av_packet_rescale_ts(avpkt, packet->encoder_time_base, st->time_base);
        LOG_DEBUG("[RTMP] After rescale: pts " + std::to_string(old_pts) + " -> " + std::to_string(avpkt->pts));
    }

    // 记录第一个视频包（必为 IDR）的 PTS 作为基准，之后所有视频包减去该基准使视频从 0 开始。
    // 上方关键帧过滤确保到达这里的第一个视频包一定是 IDR，video_pts_base_ 值即为 IDR 的 PTS。
    if (packet->type == MediaType::VIDEO) {
        if (video_pts_base_ == -1) {
            video_pts_base_ = avpkt->pts;
            LOG_INFO("[RTMP] Video PTS baseline set from IDR: " + std::to_string(video_pts_base_) + "ms");
        }
        avpkt->pts -= video_pts_base_;
        avpkt->dts -= video_pts_base_;
        // 防止负值（理论上不应发生，保险起见）
        if (avpkt->pts < 0) avpkt->pts = 0;
        if (avpkt->dts < 0) avpkt->dts = 0;
    }
    // 以第一个实际发出的音频包（IDR 之后）的 PTS 作为同步基准。
    // IDR 之前的音频已被丢弃，故此时 avpkt->pts ≈ video_pts_base_ + 一帧（~21ms），
    // 减去自身 PTS 后音频从 0ms 开始，与视频 IDR（也是 0ms）精确对齐。
    // 对重连场景同样有效：audio 在 IDR 后首次发出时 pts 约为 301600ms，
    // offset=301600ms，音视频均归零，消除 ~1730ms 的偏移。
    if (packet->type == MediaType::AUDIO && av_sync_offset_ms_ == 0 && avpkt->pts > 0) {
        av_sync_offset_ms_ = avpkt->pts;
        LOG_INFO("[RTMP] AV Sync Offset set from first sent audio: " +
                 std::to_string(av_sync_offset_ms_) + "ms");
    }

    // 音视频同步：如果音频延迟，调整音频时间戳
    if (packet->type == MediaType::AUDIO && av_sync_offset_ms_ > 0) {
        int64_t old_pts = avpkt->pts;
        int64_t old_dts = avpkt->dts;
        avpkt->pts -= av_sync_offset_ms_;
        avpkt->dts -= av_sync_offset_ms_;
        
        // 确保不为负数
        if (avpkt->pts < 0) avpkt->pts = 0;
        if (avpkt->dts < 0) avpkt->dts = 0;
        
        LOG_DEBUG("[RTMP] AV Sync applied: Adjusted audio PTS by -" +
                 std::to_string(av_sync_offset_ms_) + "ms, " +
                 std::to_string(old_pts) + "ms -> " + std::to_string(avpkt->pts) + "ms");
    }

    // 增强调试日志：打印音视频packet的完整时间戳信息（仅首帧）
    write_frame_count_++;
    if (write_frame_count_ <= 3) {
        std::string media_type = (packet->type == MediaType::VIDEO) ? "VIDEO" : "AUDIO";
        LOG_DEBUG("[RTMP] Before write: " + media_type +
                 " pts=" + std::to_string(packet->pts) +
                 ", dts=" + std::to_string(packet->dts) +
                 ", size=" + std::to_string(write_pkt->size));
    }

    // 在 write 前保存 size：av_interleaved_write_frame 会消费 packet，调用后 size 归零
    int packet_size = write_pkt->size;

    // 根据配置选择写入模式
    LOG_DEBUG("[RTMP] About to write_frame: type=" + std::string(packet->type == MediaType::AUDIO ? "AUDIO" : "VIDEO") +
              ", pts=" + std::to_string(write_pkt->pts) +
              ", size=" + std::to_string(write_pkt->size) +
              ", format_ctx=" + (format_ctx_ ? "valid" : "NULL") +
              ", pb=" + (format_ctx_ && format_ctx_->pb ? "valid" : "NULL"));
    // 通过 SEH wrapper 调用，防止 FFmpeg 内部 Access Violation 崩溃进程
    int ret = safe_write_frame(format_ctx_, write_pkt, config_.use_interleaved_write);
    LOG_DEBUG("[RTMP] write_frame returned: " + std::to_string(ret));

        if (ret < 0) {
            char errbuf[128] = {0};
            av_strerror(ret, errbuf, sizeof(errbuf));

            // AVERROR_UNKNOWN 是 SEH 捕获后的特殊值，说明 FFmpeg 内部 crash 被拦截
            bool seh_caught = (ret == AVERROR_UNKNOWN);
            LOG_ERROR("[RTMP] write_frame FAILED: ret=" + std::to_string(ret) +
                      " (" + errbuf + ")" +
                      (seh_caught ? " [SEH caught - FFmpeg internal crash intercepted]" : "") +
                      ", type=" + std::string(packet->type == MediaType::AUDIO ? "AUDIO" : "VIDEO") +
                      ", pts=" + std::to_string(packet->pts) +
                      ", format_ctx=" + (format_ctx_ ? "valid" : "NULL") +
                      ", pb=" + (format_ctx_ && format_ctx_->pb ? "valid" : "NULL"));

            if (pkt_to_free) av_packet_free(&pkt_to_free);

            // av_interleaved_write_frame 返回任何负数（含 SEH 捕获），muxer 状态均不可恢复。
            // 统一 free_resources() 并返回 NOT_CONNECTED，由 push_loop 的重连逻辑处理。
            LOG_ERROR("[RTMP] Write failure (ret=" + std::to_string(ret) +
                      "), freeing muxer state and triggering reconnect");
            free_resources();
            return ErrorCode::NOT_CONNECTED;
        }
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.bytes_sent += packet_size;
            bytes_in_window_ += packet_size;
        }

    if (pkt_to_free) {
        av_packet_free(&pkt_to_free);
    }

    return ErrorCode::SUCCESS;
}

bool RTMPPusher::is_connected() const {
    return connected_;
}

RTMPPusher::Stats RTMPPusher::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);

    Stats current_stats = stats_;
    auto now = std::chrono::steady_clock::now();

    // 滑动窗口统计：计算最近 1 秒的实时 FPS 和码率
    auto window_duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - window_start_time_);
    double window_seconds = window_duration.count() / 1000.0;

    if (window_duration.count() >= 1000) {
        // 窗口超过 1 秒，重新计算并重置窗口
        if (video_packets_in_window_ > 0 && window_seconds > 0) {
            last_calculated_fps_ = video_packets_in_window_ / window_seconds;
        }
        if (bytes_in_window_ > 0 && window_seconds > 0) {
            last_calculated_bitrate_ = (bytes_in_window_ * 8.0) / 1000.0 / window_seconds;
        }

        // 重置窗口
        window_start_time_ = now;
        video_packets_in_window_ = 0;
        audio_packets_in_window_ = 0;
        bytes_in_window_ = 0;

        // 使用刚才计算的值
        current_stats.video_fps = last_calculated_fps_;
        current_stats.bandwidth_kbps = last_calculated_bitrate_;
    } else if (window_seconds > 0) {
        // 窗口还没满 1 秒，实时计算当前窗口内的统计
        current_stats.video_fps = video_packets_in_window_ / window_seconds;
        current_stats.bandwidth_kbps = (bytes_in_window_ * 8.0) / 1000.0 / window_seconds;
    } else {
        // 窗口刚开始，使用上次计算的值
        current_stats.video_fps = last_calculated_fps_;
        current_stats.bandwidth_kbps = last_calculated_bitrate_;
    }

    // 计算音频包发送速率
    if (window_seconds > 0 && audio_packets_in_window_ > 0) {
        current_stats.audio_packets_per_sec = audio_packets_in_window_ / window_seconds;
    }

    current_stats.total_bytes_sent = stats_.bytes_sent;

    return current_stats;
}

void RTMPPusher::reset_stats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    auto now = std::chrono::steady_clock::now();
    stats_ = {connected_, 0, 0, 0, 0, 0.0, 0.0, 0.0, 0, now};

    // 重置滑动窗口统计
    window_start_time_ = now;
    video_packets_in_window_ = 0;
    audio_packets_in_window_ = 0;
    bytes_in_window_ = 0;
    last_calculated_fps_ = 0.0;
    last_calculated_bitrate_ = 0.0;
}

void RTMPPusher::free_resources() {
    LOG_INFO("[RTMP] free_resources() called: connected=" + std::to_string(connected_) +
             ", header_written=" + std::to_string(header_written_) +
             ", format_ctx=" + (format_ctx_ ? "valid" : "null") +
             ", audio_stream=" + (audio_stream_ ? "valid" : "null") +
             ", video_stream=" + (video_stream_ ? "valid" : "null"));
    if (format_ctx_) {
        audio_stream_ = nullptr;
        video_stream_ = nullptr;
        avformat_free_context(format_ctx_);
        format_ctx_ = nullptr;
        LOG_INFO("[RTMP] free_resources(): format_ctx freed");
    }

    connected_ = false;
    header_written_ = false;
    stats_.connected = false;
    LOG_INFO("[RTMP] free_resources() done");
}

ErrorCode RTMPPusher::re_register_cached_streams() {
    LOG_INFO("[RTMP] re_register_cached_streams(): cached_audio=" +
             std::string(cached_audio_codecpar_ ? "valid" : "NULL") +
             ", cached_video=" + std::string(cached_video_codecpar_ ? "valid" : "NULL") +
             ", format_ctx=" + std::string(format_ctx_ ? "valid" : "NULL"));

    if (!cached_audio_codecpar_ || !cached_video_codecpar_) {
        LOG_ERROR("[RTMP] No cached stream params for reconnect");
        return ErrorCode::INVALID_STATE;
    }

    // 重建 format context
    LOG_INFO("[RTMP] re_register: calling init_format_context()");
    ErrorCode result = init_format_context();
    if (result != ErrorCode::SUCCESS) {
        LOG_ERROR("[RTMP] Failed to re-init format context for reconnect: " +
                  std::to_string(static_cast<int>(result)));
        return result;
    }
    LOG_INFO("[RTMP] re_register: format_ctx re-initialized, registering audio stream");

    // 重新注册音频流
    audio_stream_ = avformat_new_stream(format_ctx_, nullptr);
    if (!audio_stream_) {
        LOG_ERROR("[RTMP] avformat_new_stream for audio returned null");
        return ErrorCode::INIT_FAILED;
    }
    if (avcodec_parameters_copy(audio_stream_->codecpar, cached_audio_codecpar_) < 0) {
        LOG_ERROR("[RTMP] avcodec_parameters_copy for audio failed");
        return ErrorCode::INIT_FAILED;
    }
    audio_stream_->time_base = cached_audio_time_base_;
    LOG_INFO("[RTMP] re_register: audio stream registered (index=" +
             std::to_string(audio_stream_->index) + "), registering video stream");

    // 重新注册视频流
    video_stream_ = avformat_new_stream(format_ctx_, nullptr);
    if (!video_stream_) {
        LOG_ERROR("[RTMP] avformat_new_stream for video returned null");
        return ErrorCode::INIT_FAILED;
    }
    if (avcodec_parameters_copy(video_stream_->codecpar, cached_video_codecpar_) < 0) {
        LOG_ERROR("[RTMP] avcodec_parameters_copy for video failed");
        return ErrorCode::INIT_FAILED;
    }
    video_stream_->time_base = cached_video_time_base_;

    LOG_INFO("[RTMP] Streams re-registered from cache for reconnect (audio_idx=" +
             std::to_string(audio_stream_->index) +
             ", video_idx=" + std::to_string(video_stream_->index) + ")");
    return ErrorCode::SUCCESS;
}

} // namespace live_assistant
