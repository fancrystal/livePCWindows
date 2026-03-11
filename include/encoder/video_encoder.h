#pragma once

#include <string>
#include <memory>
#include <vector>

#include "common/error.h"
#include "encoder/encoder_config.h"
#include "stream_pusher/encoded_packet.h"

struct AVCodecParameters;
struct AVRational;
struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct SwsContext;

namespace live_assistant {
struct VideoFrame;
}

namespace live_assistant {

class VideoEncoder {
public:
    virtual ~VideoEncoder() = default;

    virtual ErrorCode initialize(const VideoEncoderConfig& config) = 0;
    virtual ErrorCode shutdown() = 0;

    virtual ErrorCode encode(const std::shared_ptr<VideoFrame>& frame, std::vector<EncodedPacketPtr>& packets) = 0;

    virtual const VideoEncoderConfig& get_config() const = 0;

    // Returns a newly allocated AVCodecParameters snapshot. Caller must free via avcodec_parameters_free().
    virtual AVCodecParameters* get_codec_parameters() const = 0;
    virtual AVRational get_time_base() const = 0;

    virtual ErrorCode set_bitrate(int bitrate) = 0;
    virtual int get_bitrate() const = 0;

    virtual ErrorCode force_keyframe() = 0;
    
    virtual ErrorCode reset() { return ErrorCode::SUCCESS; }
};

class H264Encoder : public VideoEncoder {
public:
    H264Encoder();
    ~H264Encoder() override;

    ErrorCode initialize(const VideoEncoderConfig& config) override;
    ErrorCode shutdown() override;
    ErrorCode encode(const std::shared_ptr<VideoFrame>& frame, std::vector<EncodedPacketPtr>& packets) override;

    const VideoEncoderConfig& get_config() const override {
        return config_;
    }

    AVCodecParameters* get_codec_parameters() const override;
    AVRational get_time_base() const override;

    ErrorCode set_bitrate(int bitrate) override;
    int get_bitrate() const override {
        return config_.bitrate;
    }

    ErrorCode force_keyframe() override;
    ErrorCode reset() override;

    ErrorCode flush(std::vector<EncodedPacketPtr>& packets);

private:
    // Switch to next encoder in the candidate list when current encoder fails
    bool switch_to_next_encoder();
    std::string preset_to_string(VideoEncodingPreset preset) const;
    ErrorCode send_frame_internal(const std::shared_ptr<VideoFrame>& frame);
    ErrorCode send_flush();
    ErrorCode receive_packets(std::vector<EncodedPacketPtr>& packets);

    VideoEncoderConfig config_;
    bool initialized_ = false;
    bool force_keyframe_ = false;
    int64_t total_frames_ = 0;
    bool first_keyframe_sent_ = false;

    std::vector<std::string> encoder_candidates_;
    int current_encoder_index_ = 0;
    int consecutive_errors_ = 0;
    static const int MAX_CONSECUTIVE_ERRORS = 5;
    bool has_exhausted_encoders_ = false;

    const AVCodec* codec_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    AVPixelFormat sws_src_fmt_ = AV_PIX_FMT_NONE;
    int sws_src_w_ = 0;
    int sws_src_h_ = 0;

    // QSV 硬件帧上下文
    AVBufferRef* hw_device_ctx_ = nullptr;  // 硬件设备
    AVBufferRef* hw_frame_ctx_ = nullptr;  // 硬件帧池
    AVFrame* hw_frame_ = nullptr;  // 硬件帧（用于 QSV）
    AVFrame* sw_frame_ = nullptr;  // 软件帧（用于转换）

    bool is_qsv_encoder_ = false;  // 标记是否使用 QSV 编码器
};

} // namespace live_assistant
