#include "stream_pusher/push_queue.h"
#include <chrono>

namespace live_assistant {

PushQueue::PushQueue(size_t max_size) : max_size_(max_size) {
}

PushQueue::~PushQueue() {
    clear();
}

void PushQueue::set_max_size(size_t max_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_size_ = max_size;
    if (queue_.size() > max_size_) {
        discard_low_priority();
    }
}

bool PushQueue::push(EncodedPacketPtr packet) {
    if (!packet) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    
    // Check if queue is full
    if (queue_.size() >= max_size_) {
        discard_low_priority();
    }
    
    // Try to push packet
    if (queue_.size() < max_size_) {
        // Update stats
        if (packet->type == MediaType::AUDIO) {
            audio_packets_++;
        } else {
            video_packets_++;
        }
        
        queue_.push(std::move(packet));
        cv_.notify_one();
        return true;
    }
    
    // Queue is still full after discarding
    discarded_packets_++;
    return false;
}

bool PushQueue::pop(EncodedPacketPtr& packet, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    if (queue_.empty()) {
        if (timeout_ms <= 0) {
            return false;
        }
        
        if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !queue_.empty(); })) {
            return false;
        }
    }
    
    packet = queue_.top();
    queue_.pop();
    
    if (packet) {
        if (packet->type == MediaType::AUDIO) {
        audio_packets_--;
    } else {
        video_packets_--;
        }
    }
    
    return packet != nullptr;
}

bool PushQueue::try_pop(EncodedPacketPtr& packet) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (queue_.empty()) {
        return false;
    }
    
    packet = queue_.top();
    queue_.pop();
    
    if (packet) {
        if (packet->type == MediaType::AUDIO) {
        audio_packets_--;
    } else {
        video_packets_--;
        }
    }
    
    return packet != nullptr;
}

void PushQueue::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    while (!queue_.empty()) {
        queue_.pop();
    }
    
    discarded_packets_ = 0;
    audio_packets_ = 0;
    video_packets_ = 0;
    
    cv_.notify_all();
}

size_t PushQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

bool PushQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
}

bool PushQueue::full() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size() >= max_size_;
}

PushQueue::Stats PushQueue::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Stats stats;
    stats.size = queue_.size();
    stats.max_size = max_size_;
    stats.audio_packets = audio_packets_;
    stats.video_packets = video_packets_;
    stats.discarded_packets = discarded_packets_;
    
    return stats;
}

void PushQueue::discard_low_priority() {
    // 🔧 增强版丢弃策略：
    // 1. 音频：永远保留
    // 2. 视频：保留最新 I 帧及其后续所有帧，丢弃旧 GOP
    
    std::priority_queue<EncodedPacketPtr, std::vector<EncodedPacketPtr>, PacketComparator> temp_queue;
    
    // 找到队列中最新关键帧的 PTS（需要转换为微秒统一比较）
    int64_t newest_keyframe_pts_us = AV_NOPTS_VALUE;
    
    // 第一遍：找出最新关键帧的 PTS
    std::vector<EncodedPacketPtr> all_packets;
    while (!queue_.empty()) {
        auto packet = queue_.top();
        queue_.pop();
        
        if (packet && packet->type == MediaType::VIDEO && packet->is_keyframe) {
            int64_t pts_us = av_rescale_q(packet->pts, packet->encoder_time_base, AVRational{1, 1000000});
            if (newest_keyframe_pts_us == AV_NOPTS_VALUE || pts_us > newest_keyframe_pts_us) {
                newest_keyframe_pts_us = pts_us;
            }
        }
        all_packets.push_back(std::move(packet));
    }
    
    // 第二遍：根据策略决定保留或丢弃
    for (auto& packet : all_packets) {
        if (!packet) {
            continue;
        }
        
        // 1. 音频：永远保留
        if (packet->type == MediaType::AUDIO) {
            temp_queue.push(std::move(packet));
            continue;
        }
        
        // 2. 视频：检查是否应该保留
        int64_t pts_us = av_rescale_q(packet->pts, packet->encoder_time_base, AVRational{1, 1000000});
        
        // 2.1 关键帧：永远保留
        if (packet->is_keyframe) {
            temp_queue.push(std::move(packet));
            continue;
        }
        
        // 2.2 非关键帧：只保留"最新 I 帧之后"的帧
        // 如果没有找到 I 帧，则保留所有非关键帧（避免刚开始推流时丢帧）
        if (newest_keyframe_pts_us == AV_NOPTS_VALUE || pts_us >= newest_keyframe_pts_us) {
            // 在最新关键帧之后，保留
            if (temp_queue.size() < max_size_) {
                temp_queue.push(std::move(packet));
            } else {
                // 队列真的满了，统计丢弃
                discarded_packets_++;
                video_packets_--;
            }
        } else {
            // 旧 GOP（比最新 I 帧还老），丢弃
            discarded_packets_++;
            video_packets_--;
        }
    }
    
    queue_.swap(temp_queue);
}

} // namespace live_assistant
