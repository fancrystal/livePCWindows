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
struct SwrContext;

namespace live_assistant {
struct AudioFrame;
}

namespace live_assistant {

class AudioEncoder {
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

class OpusEncoder : public AudioEncoder {
public:
    OpusEncoder();
    ~OpusEncoder() override;

    ErrorCode initialize(const AudioEncoderConfig& config) override;
    ErrorCode shutdown() override;
    ErrorCode encode(const std::shared_ptr<AudioFrame>& frame, std::vector<EncodedPacketPtr>& packets) override;

    const AudioEncoderConfig& get_config() const override {
        return config_;
    }

    AVCodecParameters* get_codec_parameters() const override;
    AVRational get_time_base() const override;

    ErrorCode set_bitrate(int bitrate) override;
    int get_bitrate() const override {
        return config_.bitrate;
    }

private:
    AudioEncoderConfig config_;
    bool initialized_ = false;
    void* encoder_ = nullptr;
    AVCodecParameters* codecpar_ = nullptr;
    AVRational time_base_ = {1, 48000};
};

class AACEncoder : public AudioEncoder {
public:
    AACEncoder();
    ~AACEncoder() override;

    ErrorCode initialize(const AudioEncoderConfig& config) override;
    ErrorCode shutdown() override;
    ErrorCode encode(const std::shared_ptr<AudioFrame>& frame, std::vector<EncodedPacketPtr>& packets) override;

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

    AudioEncoderConfig config_;
    bool initialized_ = false;
    int64_t next_pts_ = 0;

    const AVCodec* codec_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    AVFrame* frame_ = nullptr;

    SwrContext* swr_ = nullptr;
    // Pending planar samples per channel used to accumulate until codec frame_size
    std::vector<std::vector<float>> pending_planar_samples_;
    int pending_samples_per_channel_ = 0;
};

} // namespace live_assistant
