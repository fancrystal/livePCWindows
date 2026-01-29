#include "encoder/encoder.h"
#include "encoder/encoder_factory.h"
#include "encoder/audio_encoder.h"
#include "encoder/video_encoder.h"
#include "common/log.h"
#include "common/error.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace live_assistant {

Encoder::Encoder() {
    LOG_INFO("Initialized Encoder");
}

Encoder::~Encoder() {
    shutdown();
    LOG_INFO("Encoder destructor called");
}

ErrorCode Encoder::initialize_video_encoder(const VideoEncoderConfig& config) {
    LOG_INFO("Initializing video encoder with configuration");
    
    video_encoder_ = EncoderFactory::create_video_encoder(config);
    if (!video_encoder_) {
        LOG_ERROR("Failed to create video encoder");
        return ErrorCode::INIT_FAILED;
    }
    
    video_config_ = config;
    video_encoder_initialized_ = true;
    
    // Verify codec parameters are available. Some hardware encoders or init paths
    // may produce an encoder that doesn't expose codec parameters usable by muxers.
    // If so, fallback to a software encoder (libx264) by disabling prefer_hw.
    AVCodecParameters* vpar = video_encoder_->get_codec_parameters();
    if (!vpar) {
        LOG_WARNING("Video encoder returned null codec parameters; attempting fallback to software encoder");
        // Shutdown and replace with software encoder
        video_encoder_->shutdown();
        video_encoder_.reset();

        VideoEncoderConfig fallback_cfg = config;
        fallback_cfg.prefer_hw = false;
        fallback_cfg.hw_accel = HWAccelerationType::NONE;
        video_encoder_ = EncoderFactory::create_video_encoder(fallback_cfg);
        if (!video_encoder_) {
            LOG_ERROR("Fallback software video encoder creation failed");
            return ErrorCode::INIT_FAILED;
        }
        video_config_ = fallback_cfg;
        vpar = video_encoder_->get_codec_parameters();
        if (!vpar) {
            LOG_ERROR("Fallback software encoder also failed to provide codec parameters");
            return ErrorCode::INIT_FAILED;
        }
        LOG_INFO("Fallback to software encoder succeeded and codec parameters are available");
    } else {
        // Free the temporary parameters returned by get_codec_parameters()
        avcodec_parameters_free(&vpar);
    }

    LOG_INFO("Video encoder initialized successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode Encoder::initialize_audio_encoder(const AudioEncoderConfig& config) {
    LOG_INFO("Initializing audio encoder with configuration");
    
    audio_encoder_ = EncoderFactory::create_audio_encoder(config);
    if (!audio_encoder_) {
        LOG_ERROR("Failed to create audio encoder");
        return ErrorCode::INIT_FAILED;
    }
    
    audio_config_ = config;
    audio_encoder_initialized_ = true;
    
    LOG_INFO("Audio encoder initialized successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode Encoder::reinitialize_video_encoder(const VideoEncoderConfig& config) {
    LOG_INFO("Reinitializing video encoder with new configuration");
    
    if (video_encoder_initialized_ && video_encoder_) {
        video_encoder_->shutdown();
        video_encoder_.reset();
    }
    
    return initialize_video_encoder(config);
}

ErrorCode Encoder::reinitialize_audio_encoder(const AudioEncoderConfig& config) {
    LOG_INFO("Reinitializing audio encoder with new configuration");
    
    if (audio_encoder_initialized_ && audio_encoder_) {
        audio_encoder_->shutdown();
        audio_encoder_.reset();
    }
    
    return initialize_audio_encoder(config);
}

ErrorCode Encoder::shutdown() {
    LOG_INFO("Shutting down encoder");
    
    if (audio_encoder_initialized_ && audio_encoder_) {
        audio_encoder_->shutdown();
        audio_encoder_.reset();
        audio_encoder_initialized_ = false;
    }
    
    if (video_encoder_initialized_ && video_encoder_) {
        video_encoder_->shutdown();
        video_encoder_.reset();
        video_encoder_initialized_ = false;
    }
    
    LOG_INFO("Encoder shutdown successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode Encoder::encode_video_frame(const std::shared_ptr<VideoFrame>& frame, std::vector<EncodedPacketPtr>& packets) {
    if (!video_encoder_initialized_ || !video_encoder_) {
        LOG_ERROR("Video encoder not initialized");
        return ErrorCode::INIT_FAILED;
    }
    
    if (!frame) {
        LOG_ERROR("Invalid video frame");
        return ErrorCode::INVALID_PARAM;
    }
    
    packets.clear();
    ErrorCode result = video_encoder_->encode(frame, packets);
    if (result != ErrorCode::SUCCESS) {
        LOG_ERROR("Failed to encode video frame");
        return result;
    }
    
    return ErrorCode::SUCCESS;
}

ErrorCode Encoder::encode_audio_frame(const std::shared_ptr<AudioFrame>& frame, std::vector<EncodedPacketPtr>& packets) {
    if (!audio_encoder_initialized_ || !audio_encoder_) {
        LOG_ERROR("Audio encoder not initialized");
        return ErrorCode::INIT_FAILED;
    }
    
    if (!frame) {
        LOG_ERROR("Invalid audio frame");
        return ErrorCode::INVALID_PARAM;
    }
    
    packets.clear();
    ErrorCode result = audio_encoder_->encode(frame, packets);
    if (result != ErrorCode::SUCCESS) {
        LOG_ERROR("Failed to encode audio frame");
        return result;
    }
    
    return ErrorCode::SUCCESS;
}

const VideoEncoderConfig& Encoder::get_video_config() const {
    return video_config_;
}

const AudioEncoderConfig& Encoder::get_audio_config() const {
    return audio_config_;
}

AVCodecParameters* Encoder::get_video_codec_parameters() const {
    if (!video_encoder_) {
        return nullptr;
    }
    return video_encoder_->get_codec_parameters();
}

AVRational Encoder::get_video_time_base() const {
    if (!video_encoder_) {
        return AVRational{0, 1};
    }
    return video_encoder_->get_time_base();
}

AVCodecParameters* Encoder::get_audio_codec_parameters() const {
    if (!audio_encoder_) {
        return nullptr;
    }
    return audio_encoder_->get_codec_parameters();
}

AVRational Encoder::get_audio_time_base() const {
    if (!audio_encoder_) {
        return AVRational{0, 1};
    }
    return audio_encoder_->get_time_base();
}

int Encoder::get_video_bitrate() const {
    if (video_encoder_) {
        return video_encoder_->get_bitrate();
    }
    return 0;
}

int Encoder::get_audio_bitrate() const {
    if (audio_encoder_) {
        return audio_encoder_->get_bitrate();
    }
    return 0;
}

ErrorCode Encoder::set_video_bitrate(int bitrate) {
    if (!video_encoder_initialized_ || !video_encoder_) {
        LOG_ERROR("Video encoder not initialized");
        return ErrorCode::INIT_FAILED;
    }
    
    video_config_.bitrate = bitrate;
    return video_encoder_->set_bitrate(bitrate);
}

ErrorCode Encoder::set_audio_bitrate(int bitrate) {
    if (!audio_encoder_initialized_ || !audio_encoder_) {
        LOG_ERROR("Audio encoder not initialized");
        return ErrorCode::INIT_FAILED;
    }
    
    audio_config_.bitrate = bitrate;
    return audio_encoder_->set_bitrate(bitrate);
}

ErrorCode Encoder::force_keyframe() {
    if (!video_encoder_initialized_ || !video_encoder_) {
        LOG_WARNING("force_keyframe called but video encoder not initialized");
        return ErrorCode::INVALID_STATE;
    }
    return video_encoder_->force_keyframe();
}

ErrorCode Encoder::reset_audio_encoder() {
    if (!audio_encoder_initialized_ || !audio_encoder_) {
        LOG_WARNING("reset_audio_encoder called but audio encoder not initialized");
        return ErrorCode::INVALID_STATE;
    }
    return audio_encoder_->reset();
}

} // namespace live_assistant
