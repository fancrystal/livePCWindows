#pragma once

#include <queue>
#include <vector>
#include <mutex>
#include <condition_variable>
#include "stream_pusher/media_packet.h"

namespace live_assistant {

// 媒体包推流队列
class PushQueue {
public:
    PushQueue(int max_size = 100);
    ~PushQueue();
    
    // 设置队列最大大小
    void set_max_size(int max_size);
    
    // 将媒体包添加到队列
    bool push(const MediaPacket& packet);
    
    // 获取下一个要发送的包 (阻塞式)
    bool pop(MediaPacket& packet, int timeout_ms = 100);
    
    // 获取下一个要发送的包 (非阻塞式)
    bool try_pop(MediaPacket& packet);
    
    // 清空队列
    void clear();
    
    // 获取队列大小
    size_t size() const;
    
    // 队列是否为空
    bool empty() const;
    
    // 队列是否已满
    bool full() const;
    
    // 获取当前队列统计信息
    struct Stats {
        size_t size;
        size_t max_size;
        int audio_packets;
        int video_packets;
        int discarded_packets;
    };
    Stats get_stats() const;
    
private:
    // 优先级队列比较器
    struct PacketComparator {
        bool operator()(const MediaPacket& a, const MediaPacket& b) {
            // 优先级高的先处理
            if (a.priority != b.priority) {
                return a.priority < b.priority;
            }
            // 旧包先处理
            return a.timestamp > b.timestamp;
        }
    };
    
    // 尝试丢弃低优先级包
    void discard_low_priority();
    
    // 队列存储
    std::priority_queue<MediaPacket, std::vector<MediaPacket>, PacketComparator> queue_;
    
    // 最大队列大小
    int max_size_;
    
    // 线程同步
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    
    // 统计信息
    mutable int discarded_packets_ = 0;
    mutable int audio_packets_ = 0;
    mutable int video_packets_ = 0;
};

} // namespace live_assistant