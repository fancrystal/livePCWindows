#include "common/circular_buffer.h"

namespace live_assistant {

CircularBuffer::CircularBuffer(size_t capacity) {
    buffer_.resize(capacity);
}

CircularBuffer::~CircularBuffer() = default;

void CircularBuffer::push_back(const void* data, size_t size) {
    if (size == 0 || !data) return;

    std::lock_guard<std::mutex> lock(mutex_);

    // 确保有足够空间
    while (size_ + size > buffer_.size()) {
        // 扩容
        size_t new_capacity = buffer_.size() * 2;
        std::vector<uint8_t> new_buffer(new_capacity);

        // 复制现有数据
        if (size_ > 0) {
            if (head_ + size_ <= buffer_.size()) {
                // 数据连续
                std::memcpy(new_buffer.data(), buffer_.data() + head_, size_);
            } else {
                // 数据不连续（环形）
                size_t first_part = buffer_.size() - head_;
                std::memcpy(new_buffer.data(), buffer_.data() + head_, first_part);
                std::memcpy(new_buffer.data() + first_part, buffer_.data(), size_ - first_part);
            }
        }

        head_ = 0;
        tail_ = size_;
        buffer_ = std::move(new_buffer);
    }

    // 写入数据
    const uint8_t* src = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        buffer_[tail_] = src[i];
        tail_ = (tail_ + 1) % buffer_.size();
    }
    size_ += size;
}

bool CircularBuffer::pop_front(void* data, size_t size) {
    if (size == 0) return true;
    if (!data || size > size_) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    uint8_t* dst = static_cast<uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        dst[i] = buffer_[head_];
        head_ = (head_ + 1) % buffer_.size();
    }
    size_ -= size;

    return true;
}

bool CircularBuffer::peek_front(void* data, size_t size) const {
    if (size == 0) return true;
    if (!data || size > size_) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    size_t pos = head_;
    uint8_t* dst = static_cast<uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        dst[i] = buffer_[pos];
        pos = (pos + 1) % buffer_.size();
    }

    return true;
}

void CircularBuffer::discard_front(size_t size) {
    if (size == 0) return;

    std::lock_guard<std::mutex> lock(mutex_);

    size_t to_discard = std::min(size, size_);
    head_ = (head_ + to_discard) % buffer_.size();
    size_ -= to_discard;
}

void CircularBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    head_ = 0;
    tail_ = 0;
    size_ = 0;
}

} // namespace live_assistant
