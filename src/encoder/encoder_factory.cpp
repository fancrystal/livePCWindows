#include "encoder/encoder_factory.h"
#include "common/log.h"

namespace live_assistant {

// Create audio encoder based on configuration
std::unique_ptr<AudioEncoder> EncoderFactory::create_audio_encoder(const AudioEncoderConfig& config) {
    std::unique_ptr<AudioEncoder> encoder;
    
    switch (config.codec) {
        case AudioCodecType::AAC:
            LOG_INFO("Creating AAC encoder");
            encoder = std::make_unique<AACEncoder>();
            break;
        default:
            LOG_ERROR("Unknown audio codec type");
            return nullptr;
    }
    
    // Initialize encoder
    ErrorCode result = encoder->initialize(config);
    if (result != ErrorCode::SUCCESS) {
        LOG_ERROR("Failed to initialize audio encoder");
        return nullptr;
    }
    
    return encoder;
}

// Create video encoder based on configuration
std::unique_ptr<VideoEncoder> EncoderFactory::create_video_encoder(const VideoEncoderConfig& config) {
    std::unique_ptr<VideoEncoder> encoder;
    
    switch (config.codec) {
        case VideoCodecType::H264:
            LOG_INFO("Creating H.264 encoder");
            encoder = std::make_unique<H264Encoder>();
            break;
        case VideoCodecType::H265:
            LOG_INFO("Creating H.265 encoder");
            // TODO: Implement H.265 encoder
            LOG_WARNING("H.265 encoder not implemented yet, falling back to H.264");
            encoder = std::make_unique<H264Encoder>();
            break;
        default:
            LOG_ERROR("Unknown video codec type");
            return nullptr;
    }
    
    // Initialize encoder
    ErrorCode result = encoder->initialize(config);
    if (result != ErrorCode::SUCCESS) {
        LOG_ERROR("Failed to initialize video encoder");
        return nullptr;
    }
    
    return encoder;
}

} // namespace live_assistant
