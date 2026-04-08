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
struct ID3D11Texture2D;

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

    // Phase 4: GPU texture 直接编码（跳过 CPU sws_scale + av_hwframe_transfer_data）
    // nv12_texture 来自 GpuColorConverter::convert()，必须与 SharedD3D11Device 同设备
    // 仅在 is_qsv_encoder_ 且 d3d11va_device_ctx_ 有效时可用
    ErrorCode encode_gpu_texture(ID3D11Texture2D* nv12_texture, int64_t pts_ms,
                                  std::vector<EncodedPacketPtr>& packets);

    bool is_gpu_texture_encode_available() const { return is_qsv_encoder_ && d3d11va_device_ctx_ != nullptr; }

private:
    // Switch to next encoder in the candidate list when current encoder fails
    bool switch_to_next_encoder();
    std::string preset_to_string(VideoEncodingPreset preset) const;
    ErrorCode send_frame_internal(const std::shared_ptr<VideoFrame>& frame);
    ErrorCode send_flush();
    ErrorCode receive_packets(std::vector<EncodedPacketPtr>& packets);

    // Phase 4: 用 SharedD3D11Device 初始化 D3D11VA 设备上下文，供 QSV 派生使用
    bool initialize_shared_d3d11va();

    // 检测系统是否有独立显卡
    // 返回：0=只有集显/软编路径, 1=NVIDIA独显, 2=AMD独显(不含集显), 3=NVIDIA+AMD都有
    int detect_discrete_gpu();

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

    // Phase 4: D3D11VA 设备/帧上下文（共享 SharedD3D11Device，供 GPU 纹理直编使用）
    AVBufferRef* d3d11va_device_ctx_ = nullptr;  // D3D11VA 设备（包装 SharedD3D11Device）
    AVBufferRef* d3d11va_frame_ctx_  = nullptr;  // D3D11VA 帧上下文（用于 av_hwframe_map 解包 QSV 帧）
};

} // namespace live_assistant
