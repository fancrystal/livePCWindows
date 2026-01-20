#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <memory>

namespace live_assistant {

struct AVPacketDeleter {
    void operator()(AVPacket* p) const {
        if (p) {
            av_packet_free(&p);
        }
    }
};

using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;

inline AVPacketPtr make_avpacket() {
    AVPacket* p = av_packet_alloc();
    return AVPacketPtr(p);
}

struct AVCodecParametersDeleter {
    void operator()(AVCodecParameters* p) const {
        if (p) {
            avcodec_parameters_free(&p);
        }
    }
};

using AVCodecParametersPtr = std::unique_ptr<AVCodecParameters, AVCodecParametersDeleter>;

inline AVCodecParametersPtr make_codecpar() {
    AVCodecParameters* p = avcodec_parameters_alloc();
    return AVCodecParametersPtr(p);
}

} // namespace live_assistant
