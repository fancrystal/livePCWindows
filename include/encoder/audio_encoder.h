#pragma once

#include <string>
#include <memory>
#include <vector>
#include <mutex>

#include <QByteArray>
#include <QObject>

#include "common/error.h"
#include "encoder/encoder_config.h"
#include "stream_pusher/encoded_packet.h"

struct AVCodecParameters;
struct AVRational;
struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct SwrContext;

namespace live_assistant {
struct AudioFrame;
}

namespace live_assistant {

class AudioEncoder : public QObject {
    Q_OBJECT
public:
    virtual ~AudioEncoder() = default;

    virtual ErrorCode initialize(const AudioEncoderConfig& config) = 0;
    virtual ErrorCode shutdown() = 0;

    virtual ErrorCode encode(const std::shared_ptr<AudioFrame>& frame, std::vector<EncodedPacketPtr>& packets) = 0;

    virtual const AudioEncoderConfig& get_config() const = 0;

    // Returns a newly allocated AVCodecParameters snapshot. Caller must free via avcodec_parameters_free().
    virtual AVCodecParameters* get_codec_parameters() const = 0;
    virtual AVRational get_time_base() const = 0;

    virtual ErrorCode set_bitrate(int bitrate) = 0;
    virtual int get_bitrate() const = 0;
    virtual ErrorCode reset() { return ErrorCode::SUCCESS; }
};

class AACEncoder : public AudioEncoder {
    Q_OBJECT
public:
    AACEncoder();
    ~AACEncoder() override;

    ErrorCode initialize(const AudioEncoderConfig& config) override;
    ErrorCode shutdown() override;
    ErrorCode encode(const std::shared_ptr<AudioFrame>& frame, std::vector<EncodedPacketPtr>& packets) override;

    // 新增：接收原始音频数据（QByteArray格式，与原项目一致）
    void encode_audio_data(const QByteArray& data, int64_t timestamp);

    const AudioEncoderConfig& get_config() const override {
        return config_;
    }

    AVCodecParameters* get_codec_parameters() const override;
    AVRational get_time_base() const override;

    ErrorCode set_bitrate(int bitrate) override;
    int get_bitrate() const override {
        return config_.bitrate;
    }

    // Flush internal encoder buffers and return remaining packets.
    ErrorCode flush(std::vector<EncodedPacketPtr>& packets);

    // Reset encoder state (for new streaming session)
    ErrorCode reset() override;

signals:
    // 编码完成后的音频数据回调（与原项目的 m_callback 对应）
    void audio_encoded(const uint8_t* data, int size, int64_t timestamp);

private:
    ErrorCode send_frame_internal(const std::shared_ptr<AudioFrame>& frame);
    ErrorCode send_flush();
    ErrorCode receive_packets(std::vector<EncodedPacketPtr>& packets);
    ErrorCode ensure_swr();
    int convert_input_data(const uint8_t* input_data, int input_size, AVFrame* frame);

    AudioEncoderConfig config_;
    bool initialized_ = false;

    const AVCodec* codec_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;

    SwrContext* swr_ = nullptr;

    // 🔧 互斥锁：保护 swr_ 和相关资源的线程安全访问
    mutable std::mutex encoder_mutex_;

    // 新增：简单的字节缓冲区（与原项目一致）
    QByteArray input_buffer_;
    int64_t last_audio_timestamp_ = -1;  // 用于单调递增保护和时间戳回绕检测
    
    // 🔧 新增：输出帧计数器，用于计算单调递增的 PTS
    // AAC 编码器可能有缓冲/延迟，不能依赖输入 timestamp 或编码器返回的 PTS
    // 我们用帧计数 * 每帧时长来计算 PTS，保证单调递增
    int64_t output_frame_count_;
    
    int64_t frame_duration_ms_;
    
    int64_t frame_offset_in_batch_;
    
    int frame_samples_;
};

} // namespace live_assistant
