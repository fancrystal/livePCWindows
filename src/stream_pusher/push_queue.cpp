#include "stream_pusher/push_queue.h"
#include <chrono>
#include <algorithm>

namespace live_assistant {

PushQueue::PushQueue(int max_size) : max_size_(max_size) {
}

PushQueue::~PushQueue() {
    clear();
}

void PushQueue::set_max_size(int max_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_size_ = max_size;
    // Discard packets if new size is smaller than current size
    if (queue_.size() > static_cast<size_t>(max_size_)) {
        discard_low_priority();
    }
}

bool PushQueue::push(const MediaPacket& packet) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Check if queue is full
    if (queue_.size() >= static_cast<size_t>(max_size_)) {
        discard_low_priority();
    }
    
    // Try to push packet
    if (queue_.size() < static_cast<size_t>(max_size_)) {
        queue_.push(packet);
        
        // Update stats
        if (packet.type == MediaType::AUDIO) {
            audio_packets_++;
        } else {
            video_packets_++;
        }
        
        cv_.notify_one();
        return true;
    }
    
    // Queue is still full after discarding
    discarded_packets_++;
    return false;
}

bool PushQueue::pop(MediaPacket& packet, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    // Wait for packet or timeout
    if (queue_.empty()) {
        if (timeout_ms <= 0) {
            return false;
        }
        
        // Wait with timeout
        if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !queue_.empty(); })) {
            return false;
        }
    }
    
    // Get packet
    packet = queue_.top();
    queue_.pop();
    
    // Update stats
    if (packet.type == MediaType::AUDIO) {
        audio_packets_--;
    } else {
        video_packets_--;
    }
    
    return true;
}

bool PushQueue::try_pop(MediaPacket& packet) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (queue_.empty()) {
        return false;
    }
    
    // Get packet
    packet = queue_.top();
    queue_.pop();
    
    // Update stats
    if (packet.type == MediaType::AUDIO) {
        audio_packets_--;
    } else {
        video_packets_--;
    }
    
    return true;
}

void PushQueue::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Clear queue
    while (!queue_.empty()) {
        queue_.pop();
    }
    
    // Reset stats
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
    return queue_.size() >= static_cast<size_t>(max_size_);
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
    // Create a temporary queue to hold kept packets
    std::priority_queue<MediaPacket, std::vector<MediaPacket>, PacketComparator> temp_queue;
    
    // Keep high-priority packets (audio and keyframes)
    while (!queue_.empty()) {
        MediaPacket packet = queue_.top();
        queue_.pop();
        
        // Always keep audio packets
        if (packet.type == MediaType::AUDIO) {
            temp_queue.push(packet);
            continue;
        }
        
        // Always keep keyframes
        if (packet.is_keyframe) {
            temp_queue.push(packet);
            continue;
        }
        
        // Keep if temp queue isn't full yet
        if (temp_queue.size() < static_cast<size_t>(max_size_ / 2)) {
            temp_queue.push(packet);
        } else {
            discarded_packets_++;
            if (packet.type == MediaType::AUDIO) {
                audio_packets_--;
            } else {
                video_packets_--;
            }
        }
    }
    
    // Replace original queue with temp queue
    queue_.swap(temp_queue);
}

} // namespace live_assistant