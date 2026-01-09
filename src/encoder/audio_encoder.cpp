#include "encoder/audio_encoder.h"
#include "common/log.h"
#include "common/error.h"
#include "audio_engine/audio_engine.h"  // For AudioFrame definition

namespace live_assistant {

// OpusEncoder implementation
OpusEncoder::OpusEncoder() : encoder_(nullptr) {
    LOG_INFO("OpusEncoder constructor");
}

OpusEncoder::~OpusEncoder() {
    if (initialized_) {
        shutdown();
    }
    LOG_INFO("OpusEncoder destructor");
}

ErrorCode OpusEncoder::initialize(const AudioEncoderConfig& config) {
    LOG_INFO("Initializing Opus encoder");
    
    // Store configuration
    config_ = config;
    
    // For now, we'll just store the configuration and mark as initialized
    // In a real implementation, we would initialize the actual encoder here
    initialized_ = true;
    
    LOG_INFO("Opus encoder initialized successfully with sample rate: " + 
              std::to_string(config.sample_rate) + "Hz, " + 
              std::to_string(config.channels) + " channels, " + 
              "bitrate: " + std::to_string(config.bitrate) + "bps");
    return ErrorCode::SUCCESS;
}

ErrorCode OpusEncoder::shutdown() {
    if (!initialized_) {
        return ErrorCode::SUCCESS;
    }
    
    LOG_INFO("Shutting down Opus encoder");
    initialized_ = false;
    LOG_INFO("Opus encoder shutdown successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode OpusEncoder::encode(const std::shared_ptr<AudioFrame>& frame, std::vector<uint8_t>& encoded_data) {
    if (!initialized_) {
        LOG_ERROR("Opus encoder not initialized");
        return ErrorCode::INIT_FAILED;
    }
    
    if (!frame) {
        LOG_ERROR("Invalid audio frame");
        return ErrorCode::INVALID_PARAM;
    }
    
    // For now, we'll just copy the raw data as-is
    // In a real implementation, we would encode the data here
    encoded_data.clear();
    int data_size = frame->samples * frame->channels * sizeof(int16_t);
    encoded_data.resize(data_size);
    memcpy(encoded_data.data(), frame->raw_data, data_size);
    
    LOG_DEBUG("Encoded Opus audio frame: " + std::to_string(encoded_data.size()) + " bytes");
    return ErrorCode::SUCCESS;
}

ErrorCode OpusEncoder::set_bitrate(int bitrate) {
    if (!initialized_) {
        return ErrorCode::INIT_FAILED;
    }
    
    config_.bitrate = bitrate;
    LOG_INFO("Set Opus bitrate to: " + std::to_string(bitrate) + "bps");
    return ErrorCode::SUCCESS;
}

// AACEncoder implementation
AACEncoder::AACEncoder() : 
    codec_(nullptr), 
    codec_ctx_(nullptr), 
    frame_(nullptr), 
    pkt_(nullptr) {
    LOG_INFO("AACEncoder constructor");
}

AACEncoder::~AACEncoder() {
    if (initialized_) {
        shutdown();
    }
    LOG_INFO("AACEncoder destructor");
}

ErrorCode AACEncoder::initialize(const AudioEncoderConfig& config) {
    LOG_INFO("Initializing AAC encoder");
    
    // Store configuration
    config_ = config;
    
    // For now, we'll just store the configuration and mark as initialized
    // In a real implementation, we would initialize the actual encoder here
    initialized_ = true;
    
    LOG_INFO("AAC encoder initialized successfully with sample rate: " + 
              std::to_string(config.sample_rate) + "Hz, " + 
              std::to_string(config.channels) + " channels, " + 
              "bitrate: " + std::to_string(config.bitrate) + "bps");
    return ErrorCode::SUCCESS;
}

ErrorCode AACEncoder::shutdown() {
    if (!initialized_) {
        return ErrorCode::SUCCESS;
    }
    
    LOG_INFO("Shutting down AAC encoder");
    initialized_ = false;
    LOG_INFO("AAC encoder shutdown successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode AACEncoder::encode(const std::shared_ptr<AudioFrame>& frame, std::vector<uint8_t>& encoded_data) {
    if (!initialized_) {
        LOG_ERROR("AAC encoder not initialized");
        return ErrorCode::INIT_FAILED;
    }
    
    if (!frame) {
        LOG_ERROR("Invalid audio frame");
        return ErrorCode::INVALID_PARAM;
    }
    
    // For now, we'll just copy the raw data as-is
    // In a real implementation, we would encode the data here
    encoded_data.clear();
    int data_size = frame->samples * frame->channels * sizeof(int16_t);
    encoded_data.resize(data_size);
    memcpy(encoded_data.data(), frame->raw_data, data_size);
    
    LOG_DEBUG("Encoded AAC audio frame: " + std::to_string(encoded_data.size()) + " bytes");
    return ErrorCode::SUCCESS;
}

ErrorCode AACEncoder::set_bitrate(int bitrate) {
    if (!initialized_) {
        return ErrorCode::INIT_FAILED;
    }
    
    config_.bitrate = bitrate;
    LOG_INFO("Set AAC bitrate to: " + std::to_string(bitrate) + "bps");
    return ErrorCode::SUCCESS;
}

} // namespace live_assistant
