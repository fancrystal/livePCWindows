#pragma once

#include <vector>
#include <cstdint>
#include <mutex>
#include <cstring>

namespace live_assistant {

// 环形缓冲区（照搬 OBS 的 deque 概念）
class CircularBuffer {
public:
    CircularBuffer(size_t capacity = 65536);
    ~CircularBuffer();

    // 压入数据
    void push_back(const void* data, size_t size);

    // 弹出数据
    bool pop_front(void* data, size_t size);

    // 查看头部数据（不弹出）
    bool peek_front(void* data, size_t size) const;

    // 丢弃头部数据
    void discard_front(size_t size);

    // 获取当前大小
    size_t size() const { return size_; }

    // 是否为空
    bool empty() const { return size_ == 0; }

    // 清空
    void clear();

    // 获取容量
    size_t capacity() const { return buffer_.size(); }

private:
    mutable std::mutex mutex_;
    std::vector<uint8_t> buffer_;
    size_t head_ = 0;  // 读取位置
    size_t tail_ = 0;  // 写入位置
    size_t size_ = 0;  // 当前数据量
};

} // namespace live_assistant
