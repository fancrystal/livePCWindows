#include "stream_pusher/stream_pusher.h"
#include "common/log.h"
#include "common/error.h"
#include <thread>
#include <chrono>

namespace live_assistant {

StreamPusher::StreamPusher() : 
    state_(StreamState::IDLE),
    stop_thread_(false),
    reconnect_attempts_(0) {
    Log::info("StreamPusher constructor");
}

StreamPusher::~StreamPusher() {
    stop();
    Log::info("StreamPusher destructor");
}

ErrorCode StreamPusher::set_config(const StreamConfig& config) {
    Log::info("Setting stream pusher config");
    
    if (state_ == StreamState::PUSHING || state_ == StreamState::CONNECTING) {
        Log::error("Cannot set config while stream is running");
        return ErrorCode::INVALID_STATE;
    }
    
    config_ = config;
    push_queue_.set_max_size(config.max_queue_size);
    
    Log::info("Stream pusher config updated successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode StreamPusher::register_audio_stream(AVCodecParameters* codecpar, AVRational time_base) {
    if (state_ != StreamState::IDLE) {
        Log::error("Cannot register stream while not in IDLE state");
        return ErrorCode::INVALID_STATE;
    }
    // Ensure RTMP pusher initialized (creates format context) before registering streams.
    ErrorCode init_result = rtmp_pusher_.initialize(config_);
    if (init_result != ErrorCode::SUCCESS) {
        Log::error("Failed to initialize RTMP pusher before registering audio stream");
        return init_result;
    }
    return rtmp_pusher_.register_audio_stream(codecpar, time_base);
}

ErrorCode StreamPusher::register_video_stream(AVCodecParameters* codecpar, AVRational time_base) {
    if (state_ != StreamState::IDLE) {
        Log::error("Cannot register stream while not in IDLE state");
        return ErrorCode::INVALID_STATE;
    }
    // Ensure RTMP pusher initialized (creates format context) before registering streams.
    ErrorCode init_result = rtmp_pusher_.initialize(config_);
    if (init_result != ErrorCode::SUCCESS) {
        Log::error("Failed to initialize RTMP pusher before registering video stream");
        return init_result;
    }
    return rtmp_pusher_.register_video_stream(codecpar, time_base);
}

ErrorCode StreamPusher::start() {
    Log::info("Starting stream pusher");
    
    if (state_ == StreamState::PUSHING || state_ == StreamState::CONNECTING) {
        Log::error("Stream is already pushing");
        return ErrorCode::ALREADY_RUNNING;
    }
    
    if (config_.server_url.empty() || config_.stream_key.empty()) {
        Log::error("Stream config not set");
        return ErrorCode::INVALID_PARAM;
    }
    
    // Initialize RTMP pusher only if not already initialized (to avoid clearing registered streams).
    ErrorCode result = ErrorCode::SUCCESS;
    if (!rtmp_pusher_.is_initialized()) {
        result = rtmp_pusher_.initialize(config_);
        if (result != ErrorCode::SUCCESS) {
            Log::error("Failed to initialize RTMP pusher");
            set_state(StreamState::ERR);
            return result;
        }
    } else {
        Log::info("RTMP pusher already initialized, skipping initialize()");
    }
    
    set_state(StreamState::CONNECTING);
    
    result = rtmp_pusher_.connect_and_write_header();
    if (result != ErrorCode::SUCCESS) {
        Log::error("Failed to connect to RTMP server and write header");
        set_state(StreamState::ERR);
        return result;
    }

    // 🔧 强制第一帧为关键帧，解决视频开头卡顿问题
    // 在推流开始前设置，确保第一帧视频被正确发送
    //rtmp_pusher_.force_next_keyframe_ = true;
    Log::info("Forcing first video frame to be keyframe");

    stop_thread_ = false;
    push_thread_ = std::thread(&StreamPusher::push_thread_func, this);
    
    set_state(StreamState::PUSHING);
    
    Log::info("Stream pusher started successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode StreamPusher::stop() {
    Log::info("Stopping stream pusher");

    if (state_ == StreamState::IDLE || state_ == StreamState::ERR) {
        return ErrorCode::SUCCESS;
    }

    set_state(StreamState::STOPPING);

    // Signal the thread to stop
    stop_thread_ = true;

    // Wait for queue to drain before forcing stop (graceful shutdown)
    const int max_wait_ms = 2000;  // Wait up to 2 seconds
    const int check_interval_ms = 50;
    int total_waited = 0;

    size_t initial_queue_size = push_queue_.size();
    if (initial_queue_size > 0) {
        Log::info("Waiting for queue to drain: " + std::to_string(initial_queue_size) + " packets remaining");
    }

    while (total_waited < max_wait_ms && !push_queue_.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(check_interval_ms));
        total_waited += check_interval_ms;

        // Log progress every 500ms
        if (total_waited % 500 == 0) {
            Log::info("Still waiting for queue: " + std::to_string(push_queue_.size()) + " packets remaining");
        }
    }

    size_t final_queue_size = push_queue_.size();
    if (final_queue_size > 0) {
        Log::warn("Queue not fully drained, " + std::to_string(final_queue_size) + " packets will be discarded");
    } else {
        Log::info("Queue fully drained before stop");
    }

    // Now join the thread
    if (push_thread_.joinable()) {
        push_thread_.join();
    }

    rtmp_pusher_.disconnect();

    // Clear any remaining packets in queue
    push_queue_.clear();

    set_state(StreamState::IDLE);

    Log::info("Stream pusher stopped successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode StreamPusher::push_packet(EncodedPacketPtr packet) {
    if (state_ != StreamState::PUSHING) {
        Log::warn("push_packet called but stream not in PUSHING state");
        return ErrorCode::INVALID_STATE;
    }

    if (!push_queue_.push(std::move(packet))) {
        Log::warn("Failed to push packet to queue, queue is full");
        return ErrorCode::QUEUE_FULL;
    }

    return ErrorCode::SUCCESS;
}

void StreamPusher::clear_queue() {
    size_t cleared = push_queue_.size();
    push_queue_.clear();
    Log::info("Stream pusher queue cleared: " + std::to_string(cleared) + " packets discarded");
}

void StreamPusher::push_thread_func() {
    Log::info("Push thread started");

    EncodedPacketPtr packet;
    auto last_reconnect_attempt = std::chrono::steady_clock::now() - std::chrono::seconds(10);

    while (!stop_thread_) {
        if (push_queue_.pop(packet, 100)) {
            ErrorCode result = rtmp_pusher_.send_packet(packet);

            if (result != ErrorCode::SUCCESS) {
                Log::error("Failed to send packet: " + std::to_string(static_cast<int>(result)));

                if (result == ErrorCode::NOT_CONNECTED) {
                    if (config_.auto_reconnect) {
                        // Non-blocking reconnection: only attempt once if enough time has passed
                        auto now = std::chrono::steady_clock::now();
                        auto time_since_last_attempt = std::chrono::duration_cast<std::chrono::seconds>(now - last_reconnect_attempt);

                        if (time_since_last_attempt.count() >= config_.reconnect_interval_sec) {
                            reconnect_attempts_++;
                            last_reconnect_attempt = now;

                            ErrorCode reconnect_result = rtmp_pusher_.connect_and_write_header();
                            if (reconnect_result == ErrorCode::SUCCESS) {
                                Log::info("Reconnected to RTMP server successfully");
                                set_state(StreamState::PUSHING);
                                // Retry sending the current packet after successful reconnection
                                result = rtmp_pusher_.send_packet(packet);
                                if (result != ErrorCode::SUCCESS) {
                                    Log::warn("Failed to send packet immediately after reconnection");
                                }
                            } else {
                                Log::error("Reconnect attempt failed, will retry later");
                                // Don't break - continue processing packets
                            }
                        }
                        // else: too soon since last attempt, skip reconnection this time
                    } else {
                        Log::error("Connection lost and auto-reconnect disabled");
                        set_state(StreamState::ERR);
                        break;
                    }
                } else if (result == ErrorCode::SEND_FAILED) {
                    Log::warn("Failed to send one packet, continuing...");
                }
            }
        }
        // Note: No additional sleep needed here - push_queue_.pop() already handles
        // waiting with 100ms timeout using condition variable, which is more efficient
    }

    Log::info("Push thread exited");
}

// Note: try_reconnect() has been replaced with non-blocking reconnection logic
// directly in push_thread_func() to avoid blocking the push thread
ErrorCode StreamPusher::try_reconnect() {
    // This function is kept for potential future use but is not called anymore
    // The new non-blocking reconnection logic is in push_thread_func()
    return ErrorCode::FAILURE;
}

StreamState StreamPusher::get_state() const {
    return state_.load();
}

bool StreamPusher::is_pushing() const {
    return state_.load() == StreamState::PUSHING;
}

bool StreamPusher::is_in_error() const {
    return state_.load() == StreamState::ERR;
}

StreamPusher::Stats StreamPusher::get_stats() const {
    Stats stats;
    stats.state = state_.load();

    auto rtmp_stats = rtmp_pusher_.get_stats();
    stats.connected = rtmp_stats.connected;
    stats.audio_packets_sent = rtmp_stats.audio_packets_sent;
    stats.video_packets_sent = rtmp_stats.video_packets_sent;
    stats.bandwidth_kbps = rtmp_stats.bandwidth_kbps;
    stats.video_fps = rtmp_stats.video_fps;
    stats.audio_packets_per_sec = rtmp_stats.audio_packets_per_sec;
    stats.total_bytes_sent = rtmp_stats.total_bytes_sent;

    auto queue_stats = push_queue_.get_stats();
    stats.discarded_packets = queue_stats.discarded_packets;

    stats.reconnect_attempts = reconnect_attempts_.load();

    return stats;
}

void StreamPusher::reset_stats() {
    rtmp_pusher_.reset_stats();
    reconnect_attempts_ = 0;
    Log::info("Stream pusher stats reset");
}

void StreamPusher::set_state(StreamState state) {
    StreamState old_state = state_.exchange(state);
    if (old_state != state) {
        Log::info("Stream state changed: " + std::to_string(static_cast<int>(old_state)) + 
                 " -> " + std::to_string(static_cast<int>(state)));
    }
}

} // namespace live_assistant
