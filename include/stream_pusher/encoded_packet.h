#pragma once

#include <cstdint>
#include <memory>
#include "common/media_clock.h"
#include "common/ffmpeg_raii.h"

namespace live_assistant {

// 媒体包类型
enum class MediaType {
    AUDIO,
    VIDEO
};

// 编码后包（跨线程安全：内部 AVPacket 由 unique_ptr 持有；队列层用 shared_ptr 传递）
struct EncodedPacket {
    MediaType type;

    // 该包在 encoder_time_base 下的时间戳
    int64_t pts = AV_NOPTS_VALUE;
    int64_t dts = AV_NOPTS_VALUE;
    int64_t duration = 0;

    // encoder time_base（用于推流侧 rescale 到 stream time_base）
    AVRational encoder_time_base{0, 1};

    bool is_keyframe = false;
    int priority = 0;

    // 便于调试/排序：原始时间（微秒），不参与 FFmpeg mux 语义
    MediaTimeUs wallclock_us = 0;

    AVPacketPtr pkt;

    EncodedPacket() = default;
};

using EncodedPacketPtr = std::shared_ptr<EncodedPacket>;

} // namespace live_assistant
