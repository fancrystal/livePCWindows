#include "common/video_frame_synchronizer.h"
#include "common/log.h"

namespace live_assistant {

VideoFrameSynchronizer::VideoFrameSynchronizer() = default;

bool VideoFrameSynchronizer::push_frame(std::shared_ptr<VideoFrame> frame, int64_t pts_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 队列满，丢帧
    if (queue_.size() >= max_size_) {
        frames_dropped_++;
        // 诊断：每丢 10 帧记录一次
        if (frames_dropped_ % 10 == 0) {
            LOG_WARNING("[VideoFrameSynchronizer] Queue full, dropped frame #" + std::to_string(frames_dropped_) +
                    " (queue_size=" + std::to_string(queue_.size()) + "/" + std::to_string(max_size_) + ")");
        }
        return false;
    }

    SyncedVideoFrame synced_frame;
    synced_frame.frame = std::move(frame);
    synced_frame.pts_ms = pts_ms;
    synced_frame.wallclock_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    queue_.push(std::move(synced_frame));
    frames_pushed_++;
    
    // 诊断：记录队列深度
    static int push_count = 0;
    push_count++;
    if (push_count % 30 == 0 && queue_.size() > 5) {  // 每 30 帧且队列深度>5 时记录
        LOG_INFO("[VideoFrameSynchronizer] Queue depth: " + std::to_string(queue_.size()) +
                "/" + std::to_string(max_size_) + ", pushed=" + std::to_string(frames_pushed_) +
                ", dropped=" + std::to_string(frames_dropped_));
    }
    
    cv_.notify_one();
    return true;
}

bool VideoFrameSynchronizer::pop_frame(SyncedVideoFrame& out_frame, int64_t timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);

    if (timeout_ms < 0) {
        // 无限等待
        cv_.wait(lock, [this] { return !queue_.empty(); });
    } else {
        // 超时等待
        auto timeout = std::chrono::milliseconds(timeout_ms);
        if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty(); })) {
            return false;  // 超时
        }
    }

    out_frame = std::move(queue_.front());
    queue_.pop();
    return true;
}

bool VideoFrameSynchronizer::try_pop_frame(SyncedVideoFrame& out_frame) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (queue_.empty()) {
        return false;
    }

    out_frame = std::move(queue_.front());
    queue_.pop();
    return true;
}

size_t VideoFrameSynchronizer::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

bool VideoFrameSynchronizer::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
}

void VideoFrameSynchronizer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
        queue_.pop();
    }
}

void VideoFrameSynchronizer::reset_stats() {
    frames_pushed_ = 0;
    frames_dropped_ = 0;
}

} // namespace live_assistant
