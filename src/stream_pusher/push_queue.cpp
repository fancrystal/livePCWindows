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
    std::priority_queue<EncodedPacketPtr, std::vector<EncodedPacketPtr>, PacketComparator> temp_queue;

    while (!queue_.empty()) {
        auto packet = queue_.top();
        queue_.pop();

        if (!packet) {
            continue;
        }

        // Always keep audio packets
        if (packet->type == MediaType::AUDIO) {
            temp_queue.push(std::move(packet));
            continue;
        }

        // Always keep keyframes
        if (packet->is_keyframe) {
            temp_queue.push(std::move(packet));
            continue;
        }

        if (temp_queue.size() < max_size_ / 2) {
            temp_queue.push(std::move(packet));
        } else {
            discarded_packets_++;
            if (packet->type == MediaType::AUDIO) {
                audio_packets_--;
            } else {
                video_packets_--;
            }
        }
    }

    queue_.swap(temp_queue);
}

} // namespace live_assistant
