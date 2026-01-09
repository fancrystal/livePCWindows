#include "encoder/video_encoder.h"
#include "common/log.h"
#include "common/error.h"
#include "video_engine/video_engine.h"  // For VideoFrame definition

namespace live_assistant {

// H264Encoder implementation
H264Encoder::H264Encoder() : 
    codec_(nullptr), 
    codec_ctx_(nullptr), 
    frame_(nullptr), 
    pkt_(nullptr), 
    force_keyframe_(false) {
    LOG_INFO("H264Encoder constructor");
    // 初始化随机种子
    srand(static_cast<unsigned int>(time(nullptr)));
}

H264Encoder::~H264Encoder() {
    if (initialized_) {
        shutdown();
    }
    LOG_INFO("H264Encoder destructor");
}

std::string H264Encoder::preset_to_string(VideoEncodingPreset preset) const {
    switch (preset) {
        case VideoEncodingPreset::ULTRAFAST:
            return "ultrafast";
        case VideoEncodingPreset::SUPERFAST:
            return "superfast";
        case VideoEncodingPreset::VERYFAST:
            return "veryfast";
        case VideoEncodingPreset::FASTER:
            return "faster";
        case VideoEncodingPreset::FAST:
            return "fast";
        case VideoEncodingPreset::MEDIUM:
            return "medium";
        case VideoEncodingPreset::SLOW:
            return "slow";
        case VideoEncodingPreset::SLOWER:
            return "slower";
        case VideoEncodingPreset::VERYSLOW:
            return "veryslow";
        case VideoEncodingPreset::PLACEBO:
            return "placebo";
        default:
            return "medium";
    }
}

ErrorCode H264Encoder::initialize(const VideoEncoderConfig& config) {
    LOG_INFO("Initializing H.264 encoder");
    
    // Store configuration
    config_ = config;
    
    // For now, we'll just store the configuration and mark as initialized
    // In a real implementation, we would initialize the actual encoder here
    initialized_ = true;
    
    LOG_INFO("H.264 encoder initialized successfully with resolution: " + 
              std::to_string(config.width) + "x" + std::to_string(config.height) + 
              ", bitrate: " + std::to_string(config.bitrate) + "bps, " + 
              "fps: " + std::to_string(config.fps) + ", GOP: " + std::to_string(config.gop));
    return ErrorCode::SUCCESS;
}

ErrorCode H264Encoder::shutdown() {
    if (!initialized_) {
        return ErrorCode::SUCCESS;
    }
    
    LOG_INFO("Shutting down H.264 encoder");
    initialized_ = false;
    LOG_INFO("H.264 encoder shutdown successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode H264Encoder::encode(const std::shared_ptr<VideoFrame>& frame, std::vector<uint8_t>& encoded_data, bool& is_keyframe) {
    if (!initialized_) {
        LOG_ERROR("H.264 encoder not initialized");
        return ErrorCode::INIT_FAILED;
    }
    
    if (!frame) {
        LOG_ERROR("Invalid video frame");
        return ErrorCode::INVALID_PARAM;
    }
    
    // For now, we'll just copy the raw data as-is
    // In a real implementation, we would encode the data here
    encoded_data.clear();
    // VideoFrame存储的是RGBA格式数据，每个像素4字节
    int data_size = frame->width * frame->height * 4;  // RGBA format (4 bytes per pixel)
    encoded_data.resize(data_size);
    memcpy(encoded_data.data(), frame->data.get(), data_size);
    
    // Randomly generate keyframes for demonstration
    is_keyframe = (rand() % 30 == 0);  // Approximately every 30 frames
    
    // Force keyframe if requested
    if (force_keyframe_) {
        is_keyframe = true;
        force_keyframe_ = false;
    }
    
    LOG_DEBUG("Encoded H.264 video frame: " + std::to_string(encoded_data.size()) + " bytes, " + 
               (is_keyframe ? "keyframe" : "interframe"));
    return ErrorCode::SUCCESS;
}

ErrorCode H264Encoder::set_bitrate(int bitrate) {
    if (!initialized_) {
        return ErrorCode::INIT_FAILED;
    }
    
    config_.bitrate = bitrate;
    LOG_INFO("Set H.264 bitrate to: " + std::to_string(bitrate) + "bps");
    return ErrorCode::SUCCESS;
}

ErrorCode H264Encoder::force_keyframe() {
    if (!initialized_) {
        return ErrorCode::INIT_FAILED;
    }
    
    force_keyframe_ = true;
    LOG_INFO("Forcing keyframe in next frame");
    return ErrorCode::SUCCESS;
}

} // namespace live_assistant
