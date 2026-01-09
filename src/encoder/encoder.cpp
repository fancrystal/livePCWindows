#include "encoder/encoder.h"
#include "encoder/encoder_factory.h"
#include "common/log.h"
#include "common/error.h"

namespace live_assistant {

Encoder::Encoder() {
    LOG_INFO("Initialized Encoder");
}

Encoder::~Encoder() {
    shutdown();
    LOG_INFO("Encoder destructor called");
}

// Initialize video encoder with configuration
ErrorCode Encoder::initialize_video_encoder(const VideoEncoderConfig& config) {
    LOG_INFO("Initializing video encoder with configuration");
    
    // Create video encoder using factory
    video_encoder_ = EncoderFactory::create_video_encoder(config);
    if (!video_encoder_) {
        LOG_ERROR("Failed to create video encoder");
        return ErrorCode::INIT_FAILED;
    }
    
    // Store configuration
    video_config_ = config;
    video_encoder_initialized_ = true;
    
    LOG_INFO("Video encoder initialized successfully");
    return ErrorCode::SUCCESS;
}

// Initialize audio encoder with configuration
ErrorCode Encoder::initialize_audio_encoder(const AudioEncoderConfig& config) {
    LOG_INFO("Initializing audio encoder with configuration");
    
    // Create audio encoder using factory
    audio_encoder_ = EncoderFactory::create_audio_encoder(config);
    if (!audio_encoder_) {
        LOG_ERROR("Failed to create audio encoder");
        return ErrorCode::INIT_FAILED;
    }
    
    // Store configuration
    audio_config_ = config;
    audio_encoder_initialized_ = true;
    
    LOG_INFO("Audio encoder initialized successfully");
    return ErrorCode::SUCCESS;
}

// Reinitialize video encoder with new configuration
ErrorCode Encoder::reinitialize_video_encoder(const VideoEncoderConfig& config) {
    LOG_INFO("Reinitializing video encoder with new configuration");
    
    // Shutdown current encoder if it exists
    if (video_encoder_initialized_ && video_encoder_) {
        video_encoder_->shutdown();
        video_encoder_.reset();
    }
    
    // Initialize with new configuration
    return initialize_video_encoder(config);
}

// Reinitialize audio encoder with new configuration
ErrorCode Encoder::reinitialize_audio_encoder(const AudioEncoderConfig& config) {
    LOG_INFO("Reinitializing audio encoder with new configuration");
    
    // Shutdown current encoder if it exists
    if (audio_encoder_initialized_ && audio_encoder_) {
        audio_encoder_->shutdown();
        audio_encoder_.reset();
    }
    
    // Initialize with new configuration
    return initialize_audio_encoder(config);
}

ErrorCode Encoder::shutdown() {
    LOG_INFO("Shutting down encoder");
    
    // Shutdown audio encoder
    if (audio_encoder_initialized_ && audio_encoder_) {
        audio_encoder_->shutdown();
        audio_encoder_.reset();
        audio_encoder_initialized_ = false;
    }
    
    // Shutdown video encoder
    if (video_encoder_initialized_ && video_encoder_) {
        video_encoder_->shutdown();
        video_encoder_.reset();
        video_encoder_initialized_ = false;
    }
    
    LOG_INFO("Encoder shutdown successfully");
    return ErrorCode::SUCCESS;
}

// Encode video frame
ErrorCode Encoder::encode_video_frame(const std::shared_ptr<VideoFrame>& frame, std::vector<uint8_t>& encoded_data) {
    if (!video_encoder_initialized_ || !video_encoder_) {
        LOG_ERROR("Video encoder not initialized");
        return ErrorCode::INIT_FAILED;
    }
    
    if (!frame) {
        LOG_ERROR("Invalid video frame");
        return ErrorCode::INVALID_PARAM;
    }
    
    // Encode frame using video encoder
    bool is_keyframe = false;
    ErrorCode result = video_encoder_->encode(frame, encoded_data, is_keyframe);
    if (result != ErrorCode::SUCCESS) {
        LOG_ERROR("Failed to encode video frame");
        return result;
    }
    
    // Log keyframe information
    if (is_keyframe) {
        LOG_DEBUG("Encoded video keyframe");
    } else {
        LOG_DEBUG("Encoded video frame");
    }
    
    return ErrorCode::SUCCESS;
}

// Encode audio frame
ErrorCode Encoder::encode_audio_frame(const std::shared_ptr<AudioFrame>& frame, std::vector<uint8_t>& encoded_data) {
    if (!audio_encoder_initialized_ || !audio_encoder_) {
        LOG_ERROR("Audio encoder not initialized");
        return ErrorCode::INIT_FAILED;
    }
    
    if (!frame) {
        LOG_ERROR("Invalid audio frame");
        return ErrorCode::INVALID_PARAM;
    }
    
    // Encode frame using audio encoder
    ErrorCode result = audio_encoder_->encode(frame, encoded_data);
    if (result != ErrorCode::SUCCESS) {
        LOG_ERROR("Failed to encode audio frame");
        return result;
    }
    
    LOG_DEBUG("Encoded audio frame");
    return ErrorCode::SUCCESS;
}

// Get video encoder configuration
const VideoEncoderConfig& Encoder::get_video_config() const {
    return video_config_;
}

// Get audio encoder configuration
const AudioEncoderConfig& Encoder::get_audio_config() const {
    return audio_config_;
}

// Get current video bitrate
int Encoder::get_video_bitrate() const {
    if (video_encoder_) {
        return video_encoder_->get_bitrate();
    }
    return 0;
}

// Get current audio bitrate
int Encoder::get_audio_bitrate() const {
    if (audio_encoder_) {
        return audio_encoder_->get_bitrate();
    }
    return 0;
}

// Set video bitrate dynamically
ErrorCode Encoder::set_video_bitrate(int bitrate) {
    if (!video_encoder_initialized_ || !video_encoder_) {
        LOG_ERROR("Video encoder not initialized");
        return ErrorCode::INIT_FAILED;
    }
    
    // Update configuration
    video_config_.bitrate = bitrate;
    
    // Set bitrate on encoder
    return video_encoder_->set_bitrate(bitrate);
}

// Set audio bitrate dynamically
ErrorCode Encoder::set_audio_bitrate(int bitrate) {
    if (!audio_encoder_initialized_ || !audio_encoder_) {
        LOG_ERROR("Audio encoder not initialized");
        return ErrorCode::INIT_FAILED;
    }
    
    // Update configuration
    audio_config_.bitrate = bitrate;
    
    // Set bitrate on encoder
    return audio_encoder_->set_bitrate(bitrate);
}

} // namespace live_assistant
