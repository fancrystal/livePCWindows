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
    // 快速丢弃策略：从队列中丢弃一个最旧的、可丢弃的包
    // 只遍历一次，找到最旧的且非关键帧音频的包丢弃
    
    // 将所有包取出，丢弃最旧的非关键帧视频包，其余放回
    std::vector<EncodedPacketPtr> all_packets;
    while (!queue_.empty()) {
        all_packets.push_back(std::move(const_cast<EncodedPacketPtr&>(queue_.top())));
        queue_.pop();
    }
    
    // 找到要丢弃的候选：最旧的非关键帧视频包（最后一个元素是PTS最小的）
    int discard_idx = -1;
    for (int i = static_cast<int>(all_packets.size()) - 1; i >= 0; --i) {
        auto& pkt = all_packets[i];
        if (pkt && pkt->type == MediaType::VIDEO && !pkt->is_keyframe) {
            discard_idx = i;
            break;  // 优先队列最后一个是最旧的，直接丢弃
        }
    }
    
    if (discard_idx >= 0) {
        discarded_packets_++;
        video_packets_--;
        all_packets.erase(all_packets.begin() + discard_idx);
    }
    // 如果全是音频和关键帧，丢弃最旧的非音频包
    else if (!all_packets.empty()) {
        // 找最旧的包丢弃
        for (int i = static_cast<int>(all_packets.size()) - 1; i >= 0; --i) {
            if (all_packets[i] && all_packets[i]->type == MediaType::VIDEO) {
                discarded_packets_++;
                video_packets_--;
                all_packets.erase(all_packets.begin() + i);
                break;
            }
        }
    }
    
    // 放回队列
    for (auto& pkt : all_packets) {
        if (pkt) {
            queue_.push(std::move(pkt));
        }
    }
}

} // namespace live_assistant
