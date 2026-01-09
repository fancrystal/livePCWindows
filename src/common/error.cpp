#include "common/error.h"

namespace live_assistant {
    
std::string Error::to_string(ErrorCode code) {
    switch (code) {
        case ErrorCode::SUCCESS:
            return "Success";
        case ErrorCode::FAILURE:
            return "General failure";
        case ErrorCode::INIT_FAILED:
            return "Initialization failed";
        case ErrorCode::INVALID_PARAM:
            return "Invalid parameter";
        case ErrorCode::NOT_SUPPORTED:
            return "Not supported";
        case ErrorCode::NOT_FOUND:
            return "Not found";
        case ErrorCode::NETWORK_ERROR:
            return "Network error";
        case ErrorCode::ENCODING_ERROR:
            return "Encoding error";
        case ErrorCode::DEVICE_ERROR:
            return "Device error";
        case ErrorCode::STREAM_ERROR:
            return "Stream error";
        case ErrorCode::CODEC_ERROR:
            return "Codec error";
        case ErrorCode::AUDIO_INIT_ERROR:
            return "Audio initialization error";
        case ErrorCode::AUDIO_DEVICE_ERROR:
            return "Audio device error";
        case ErrorCode::AUDIO_FORMAT_ERROR:
            return "Audio format error";
        default:
            return "Unknown error";
    }
}

} // namespace live_assistant
