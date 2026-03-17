#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <chrono>
#include <atomic>

#include "video_engine/video_engine.h"

namespace live_assistant {

struct SyncedVideoFrame {
    std::shared_ptr<VideoFrame> frame;
    int64_t pts_ms;           // 基于直播流时间轴的 PTS
    int64_t wallclock_us;     // 接收时的壁挂钟时间
};

class VideoFrameSynchronizer {
public:
    VideoFrameSynchronizer();
    ~VideoFrameSynchronizer() = default;

    // 帧生产者调用：放入帧
    // 返回 false 表示队列满，需要丢帧
    bool push_frame(std::shared_ptr<VideoFrame> frame, int64_t pts_ms);

    // 帧消费者调用：获取帧
    // timeout_ms: 最大等待时间，-1 表示无限等待
    // 返回 false 表示超时
    bool pop_frame(SyncedVideoFrame& out_frame, int64_t timeout_ms = 33);

    // 尝试获取帧（非阻塞）
    // 返回 false 表示队列为空
    bool try_pop_frame(SyncedVideoFrame& out_frame);

    // 队列状态
    size_t size() const;
    bool empty() const;

    // 丢弃队列中的所有帧（用于切换/停止）
    void clear();

    // 设置最大队列大小
    void set_max_size(size_t max_size) { max_size_ = max_size; }

    // 获取最大队列大小
    size_t max_size() const { return max_size_; }

    // 统计数据
    uint64_t frames_pushed() const { return frames_pushed_.load(); }
    uint64_t frames_dropped() const { return frames_dropped_.load(); }
    void reset_stats();

private:
    std::queue<SyncedVideoFrame> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    size_t max_size_ = 8;  // 默认 8 帧缓冲（约 267ms @ 30fps）

    std::atomic<uint64_t> frames_pushed_{0};
    std::atomic<uint64_t> frames_dropped_{0};
};

} // namespace live_assistant
