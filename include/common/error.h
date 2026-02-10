#pragma once

#include <string>

namespace live_assistant {

enum class ErrorCode {
    SUCCESS = 0,
    FAILURE = 1,
    INIT_FAILED = 2,
    INVALID_PARAM = 3,
    NOT_SUPPORTED = 4,
    NOT_FOUND = 5,
    NETWORK_ERROR = 6,
    ENCODING_ERROR = 7,
    DEVICE_ERROR = 8,
    STREAM_ERROR = 9,
    CODEC_ERROR = 10,
    AUDIO_INIT_ERROR = 11,
    AUDIO_DEVICE_ERROR = 12,
    AUDIO_FORMAT_ERROR = 13,
    INVALID_STATE = 14,
    ALREADY_RUNNING = 15,
    QUEUE_FULL = 16,
    NOT_CONNECTED = 17,
    SEND_FAILED = 18,
    CONNECT_FAILED = 19,
    ALREADY_INITIALIZED = 20,
    ALREADY_EXISTS = 21
};

class Error {
public:
    static std::string to_string(ErrorCode code);
};

} // namespace live_assistant
