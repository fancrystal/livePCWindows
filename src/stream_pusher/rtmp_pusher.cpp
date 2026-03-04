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

namespace live_assistant {

RTMPPusher::RTMPPusher() {
    avformat_network_init();
    LOG_INFO("RTMPPusher constructor");
}

RTMPPusher::~RTMPPusher() {
    disconnect();
    free_resources();
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

    video_stream_ = avformat_new_stream(format_ctx_, nullptr);
    if (!video_stream_) {
        LOG_ERROR("Failed to create video stream");
        return ErrorCode::INIT_FAILED;
    }
    
    if (avcodec_parameters_copy(video_stream_->codecpar, codecpar) < 0) {
        LOG_ERROR("Failed to copy video codec parameters");
        return ErrorCode::INIT_FAILED;
    }

    video_stream_->time_base = time_base;
    return ErrorCode::SUCCESS;
}

ErrorCode RTMPPusher::open_output() {
    if (!format_ctx_) {
        return ErrorCode::INIT_FAILED;
    }

    std::string full_url = config_.server_url + "/" + config_.stream_key;
    // 调试模式：可选择输出到本地文件进行测试
    // 要测试本地文件，请取消下面一行的注释：
    full_url = "D:/test.flv";
    // 正常推流时，请确保这一行被注释掉

    // 对于本地文件测试，先删除已存在的文件以确保干净输出
    // RTMP 推流时服务器会自动处理
    if (full_url.find(".flv") != std::string::npos) {
        std::remove(full_url.c_str());  // 删除已存在的文件
    }

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
    if (connected_ && header_written_) {
        return ErrorCode::SUCCESS;
    }

    if (!audio_stream_ || !video_stream_) {
        LOG_ERROR("Audio/video streams not registered before connect_and_write_header");
        return ErrorCode::INVALID_STATE;
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

    header_written_ = true;
    is_first_video_packet_ = true;
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

    // 🔧 调试日志：打印每一帧的 PTS（音频和视频）
    if (packet->type == MediaType::AUDIO) {
        static int audio_frame_count = 0;
        audio_frame_count++;
        LOG_INFO("[DEBUG] AUDIO frame #" + std::to_string(audio_frame_count) + 
                 ": pts=" + std::to_string(packet->pts) + "ms" +
                 ", dts=" + std::to_string(packet->dts) + "ms" +
                 ", duration=" + std::to_string(packet->duration) + "ms" +
                 ", wallclock=" + std::to_string(packet->wallclock_us / 1000) + "ms");
    } else {
        static int video_frame_count = 0;
        video_frame_count++;
        LOG_INFO("[DEBUG] VIDEO frame #" + std::to_string(video_frame_count) + 
                 ": pts=" + std::to_string(packet->pts) + "ms" +
                 ", dts=" + std::to_string(packet->dts) + "ms" +
                 ", duration=" + std::to_string(packet->duration) + "ms" +
                 ", wallclock=" + std::to_string(packet->wallclock_us / 1000) + "ms" +
                 ", is_keyframe=" + std::to_string(packet->is_keyframe ? 1 : 0));
    }

    // 🔧 诊断第一帧 PTS 问题
    static int64_t first_audio_pts = -1;
    static int64_t first_video_pts = -1;
    static int64_t first_audio_wallclock = -1;
    static int64_t first_video_wallclock = -1;
    
    if (packet->type == MediaType::AUDIO && first_audio_pts == -1) {
        first_audio_pts = packet->pts;
        first_audio_wallclock = packet->wallclock_us / 1000;
        LOG_INFO("[RTMP] First AUDIO packet: pts=" + std::to_string(packet->pts) + 
                 ", wallclock=" + std::to_string(first_audio_wallclock) + "ms" +
                 ", dts=" + std::to_string(packet->dts));
    } else if (packet->type == MediaType::VIDEO && first_video_pts == -1) {
        first_video_pts = packet->pts;
        first_video_wallclock = packet->wallclock_us / 1000;
        LOG_INFO("[RTMP] First VIDEO packet: pts=" + std::to_string(packet->pts) + 
                 ", wallclock=" + std::to_string(first_video_wallclock) + "ms" +
                 ", dts=" + std::to_string(packet->dts));
        
        // 打印音视频第一帧的对比
        if (first_audio_pts != -1) {
            LOG_INFO("[RTMP] AV Sync Check: first_audio_pts=" + std::to_string(first_audio_pts) + 
                     "ms, first_video_pts=" + std::to_string(first_video_pts) + "ms" +
                     ", diff=" + std::to_string(first_video_pts - first_audio_pts) + "ms");
        }
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

    // Determine target stream and its time_base
    AVStream* st = nullptr;
    if (packet->type == MediaType::AUDIO) {
        if (!audio_stream_) {
            return ErrorCode::INVALID_STATE;
        }
        st = audio_stream_;
        
        // Log audio packet details for debugging
        static int audio_packet_count = 0;
        audio_packet_count++;
        if (audio_packet_count % 50 == 0) {
            LOG_INFO("[RTMP] Audio packet #" + std::to_string(audio_packet_count) + 
                     ": pts=" + std::to_string(packet->pts) + 
                     ", duration=" + std::to_string(packet->duration) +
                     ", size=" + std::to_string(packet->pkt ? packet->pkt->size : 0) +
                     ", stream_time_base=" + std::to_string(st->time_base.num) + "/" + std::to_string(st->time_base.den));
        }
        
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.audio_packets_sent++;
            last_audio_packet_time_ = std::chrono::steady_clock::now();
            audio_packets_since_last_calc_++;
        }
    } else {
        if (!video_stream_) {
            return ErrorCode::INVALID_STATE;
        }
        st = video_stream_;
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.video_packets_sent++;
            last_video_packet_time_ = std::chrono::steady_clock::now();
            video_packets_since_last_calc_++;
        }

        // If caller marked packet as keyframe, respect it
        if (packet->is_keyframe) {
            avpkt->flags |= AV_PKT_FLAG_KEY;
        }

        // If we were requested to force next keyframe, mark it
        if (force_next_keyframe_.exchange(false)) {
            avpkt->flags |= AV_PKT_FLAG_KEY;
        }

        // 🔧 修复：确保第一帧视频是关键帧，但最多只等待100ms
        // 这样可以避免视频延迟，同时确保第一帧质量
        static auto first_video_wait_start = std::chrono::steady_clock::now();
        static bool first_video_wait_initialized = false;

        if (packet->type == MediaType::VIDEO && !have_sent_first_key_) {
            if (!(avpkt->flags & AV_PKT_FLAG_KEY)) {
                // 初始化等待计时器
                if (!first_video_wait_initialized) {
                    first_video_wait_start = std::chrono::steady_clock::now();
                    first_video_wait_initialized = true;
                }

                // 计算已等待时间
                auto elapsed = std::chrono::steady_clock::now() - first_video_wait_start;
                auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

                // 最多等待100ms，超过则接受非关键帧
                if (elapsed_ms < 100) {
                    LOG_WARNING("[RTMP] Dropping non-key video packet, waiting for keyframe (" +
                               std::to_string(elapsed_ms) + "ms elapsed)");
                    return ErrorCode::SUCCESS;
                } else {
                    LOG_WARNING("[RTMP] Timeout waiting for keyframe, accepting non-key frame after " +
                               std::to_string(elapsed_ms) + "ms");
                    have_sent_first_key_ = true;
                    first_video_wait_initialized = false;
                }
            } else {
                LOG_INFO("[RTMP] First keyframe received, size=" + std::to_string(avpkt->size));
                have_sent_first_key_ = true;
                first_video_wait_initialized = false;
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

    if (packet->type == MediaType::VIDEO && is_first_video_packet_) {
        if (avpkt->pts > 0) {
            LOG_INFO("[RTMP] Fixing first video PTS: " + std::to_string(avpkt->pts) + "ms -> 0ms");
            avpkt->pts = 0;
            avpkt->dts = 0;
        }
        is_first_video_packet_ = false;
    }

    // Note: Packet was already cloned at the beginning of this function
    // write_pkt and pkt_to_free are already set

    // For video packets, check if we need fragmentation (only for AVCC format)
    bool fragmented = false;
    if (packet->type == MediaType::VIDEO && st->codecpar &&
        st->codecpar->codec_id == AV_CODEC_ID_H264 &&
        st->codecpar->extradata && st->codecpar->extradata_size >= 5 &&
        write_pkt->size > 64 * 1024) { // Only fragment packets > 64KB

        // Check nal_length_size (usually 4 for AVCC)
        uint8_t nal_length_size = st->codecpar->extradata[4] & 0x03;
        if (nal_length_size == 3) nal_length_size = 4; // 3 means 4 bytes

        if (nal_length_size == 4) {
            fragmented = true;
            LOG_DEBUG("RTMPPusher::send_packet - fragmenting large AVCC packet, size=" + std::to_string(write_pkt->size));

            // Fragment the packet
            const uint8_t* data = write_pkt->data;
            size_t remaining = write_pkt->size;
            size_t offset = 0;

            while (remaining >= 4) {
                // Read NAL length (big-endian)
                uint32_t nal_len = (data[offset] << 24) | (data[offset + 1] << 16) | (data[offset + 2] << 8) | data[offset + 3];
                offset += 4;

                if (nal_len == 0 || nal_len > remaining - 4) {
                    LOG_WARNING("RTMPPusher::send_packet - invalid NAL length " + std::to_string(nal_len) + " at offset " + std::to_string(offset - 4));
                    fragmented = false;
                    break;
                }

                // Create fragment packet
                AVPacket* frag_pkt = av_packet_alloc();
                if (!frag_pkt) {
                    LOG_ERROR("RTMPPusher::send_packet - failed to alloc fragment packet");
                    fragmented = false;
                    break;
                }

                // Copy packet metadata
                av_packet_ref(frag_pkt, write_pkt);
                frag_pkt->data = write_pkt->data + offset - 4; // Include length prefix
                frag_pkt->size = nal_len + 4;
                frag_pkt->stream_index = write_pkt->stream_index;

                // 根据配置选择写入模式
                int ret;
                if (config_.use_interleaved_write) {
                    ret = av_interleaved_write_frame(format_ctx_, frag_pkt);
                } else {
                    ret = av_write_frame(format_ctx_, frag_pkt);
                }
                av_packet_free(&frag_pkt);

                if (ret < 0) {
                    char errbuf[128] = {0};
                    av_strerror(ret, errbuf, sizeof(errbuf));
                    LOG_ERROR("RTMPPusher::send_packet - failed to send fragment: " + std::string(errbuf));
                    fragmented = false;
                    break;
                }

                offset += nal_len;
                remaining -= (nal_len + 4);
                stats_.bytes_sent += (nal_len + 4);
            }

            if (!fragmented) {
                LOG_WARNING("RTMPPusher::send_packet - fragmentation failed, falling back to sending whole packet");
            }
        }
    }

    // Send the original packet if not fragmented or fragmentation failed
    if (!fragmented) {
        // 🔧 增强调试日志：打印音视频packet的完整时间戳信息
        std::string media_type = (packet->type == MediaType::VIDEO) ? "VIDEO" : "AUDIO";
        LOG_INFO("[RTMP] Before write: " + media_type +
                 " pts=" + std::to_string(packet->pts) +
                 ", dts=" + std::to_string(packet->dts) +
                 ", duration=" + std::to_string(packet->duration) +
                 ", encoder_tb=" + std::to_string(packet->encoder_time_base.num) + "/" + std::to_string(packet->encoder_time_base.den) +
                 ", stream_tb=" + std::to_string(st->time_base.num) + "/" + std::to_string(st->time_base.den) +
                 ", stream_index=" + std::to_string(write_pkt->stream_index));
        
        LOG_DEBUG("[RTMP] Before write: pts=" + std::to_string(write_pkt->pts) +
                  ", dts=" + std::to_string(write_pkt->dts) +
                  ", size=" + std::to_string(write_pkt->size) +
                  ", duration=" + std::to_string(write_pkt->duration) +
                  ", stream_index=" + std::to_string(write_pkt->stream_index));

        // 根据配置选择写入模式
        int ret;
        if (config_.use_interleaved_write) {
            ret = av_interleaved_write_frame(format_ctx_, write_pkt);
        } else {
            ret = av_write_frame(format_ctx_, write_pkt);
        }

        LOG_DEBUG("[RTMP] After write: ret=" + std::to_string(ret) +
                  ", pts=" + std::to_string(write_pkt->pts) +
                  ", dts=" + std::to_string(write_pkt->dts) +
                  ", size=" + std::to_string(write_pkt->size) +
                  ", mode=" + std::string(config_.use_interleaved_write ? "interleaved" : "direct"));
        
        // 🔧 增强调试日志：打印rescale后的时间戳
        int64_t pts_in_ms = (write_pkt->pts != AV_NOPTS_VALUE && st->time_base.den > 0) 
            ? (write_pkt->pts * 1000 * st->time_base.num / st->time_base.den) : -1;
        LOG_INFO("[RTMP] After rescale: " + media_type +
                 " pts=" + std::to_string(write_pkt->pts) +
                 ", dts=" + std::to_string(write_pkt->dts) +
                 ", pts_ms=" + std::to_string(pts_in_ms) + "ms");

        // Save size before potential free
        int packet_size = write_pkt->size;

        if (ret < 0) {
            char errbuf[128] = {0};
            av_strerror(ret, errbuf, sizeof(errbuf));

            // Log detailed debug info for audio packets
            if (packet->type == MediaType::AUDIO) {
                LOG_ERROR("[RTMP] Audio send failed: ret=" + std::to_string(ret) + " (" + errbuf + ")");
                LOG_ERROR("  - pts=" + std::to_string(write_pkt->pts) + ", dts=" + std::to_string(write_pkt->dts));
                LOG_ERROR("  - duration=" + std::to_string(write_pkt->duration) + ", size=" + std::to_string(packet_size));
                LOG_ERROR("  - stream_index=" + std::to_string(write_pkt->stream_index));
                LOG_ERROR("  - stream.time_base=" + std::to_string(st->time_base.num) + "/" + std::to_string(st->time_base.den));
                //LOG_ERROR("  - codecpar.channels=" + std::to_string(st->codecpar->channels));
                LOG_ERROR("  - codecpar.sample_rate=" + std::to_string(st->codecpar->sample_rate));
                LOG_ERROR("  - codecpar.extradata_size=" + std::to_string(st->codecpar->extradata_size));
            }

            LOG_ERROR(std::string("Failed to send packet, av_interleaved_write_frame returned ") + std::to_string(ret) + ": " + errbuf);
            if (pkt_to_free) av_packet_free(&pkt_to_free);
            return ErrorCode::SEND_FAILED;
        }
        stats_.bytes_sent += packet_size;
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

    // 计算运行时间
    auto total_duration = std::chrono::duration_cast<std::chrono::seconds>(now - stats_.start_time);
    if (total_duration.count() > 0) {
        // 计算带宽 (kbps)
        current_stats.bandwidth_kbps = static_cast<double>(stats_.bytes_sent * 8) / 1000.0 / total_duration.count();

        // 计算视频帧率 (基于最近的包发送情况)
        auto video_duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_video_packet_time_);
        if (video_duration.count() > 0 && video_packets_since_last_calc_ > 0) {
            current_stats.video_fps = static_cast<double>(video_packets_since_last_calc_) / video_duration.count();
        }

        // 计算音频包发送速率
        auto audio_duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_audio_packet_time_);
        if (audio_duration.count() > 0 && audio_packets_since_last_calc_ > 0) {
            current_stats.audio_packets_per_sec = static_cast<double>(audio_packets_since_last_calc_) / audio_duration.count();
        }
    }

    current_stats.total_bytes_sent = stats_.bytes_sent;

    return current_stats;
}

void RTMPPusher::reset_stats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    auto now = std::chrono::steady_clock::now();
    stats_ = {connected_, 0, 0, 0, 0, 0.0, 0.0, 0.0, 0, now};
    last_video_packet_time_ = now;
    last_audio_packet_time_ = now;
    video_packets_since_last_calc_ = 0;
    audio_packets_since_last_calc_ = 0;
}

void RTMPPusher::free_resources() {
    if (format_ctx_) {
        audio_stream_ = nullptr;
        video_stream_ = nullptr;
        avformat_free_context(format_ctx_);
        format_ctx_ = nullptr;
    }
    
    connected_ = false;
    header_written_ = false;
    stats_.connected = false;
}

} // namespace live_assistant
