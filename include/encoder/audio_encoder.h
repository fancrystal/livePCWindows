#pragma once

#include <string>
#include <memory>
#include <vector>

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

    // Reset encoder state (for new streaming session)
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
    void write_samples_to_frame(const std::vector<std::vector<float>>& pending,
                                int samples_to_write,
                                AVFrame* frame,
                                int fmt,
                                int channels);

    // 新增：处理原始音频数据（与原项目一致）
    void process_audio_data(const QByteArray& data, int64_t timestamp);
    int convert_input_data(const uint8_t* input_data, int input_size, AVFrame* frame);

    AudioEncoderConfig config_;
    bool initialized_ = false;
    int64_t next_pts_ = 0;

    const AVCodec* codec_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;

    SwrContext* swr_ = nullptr;

    // 新增：简单的字节缓冲区（与原项目一致）
    QByteArray input_buffer_;
    int64_t last_audio_timestamp_ = -1;  // 用于单调递增保护
};

} // namespace live_assistant
