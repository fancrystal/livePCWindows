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

    StreamState current_state = state_.load();
    if (current_state == StreamState::IDLE && !push_thread_.joinable()) {
        return ErrorCode::SUCCESS;
    }

    if (current_state != StreamState::IDLE && current_state != StreamState::STOPPING) {
        set_state(StreamState::STOPPING);
    }

    // Signal the thread to stop
    stop_thread_ = true;

    // Low-latency stop: discard queued history immediately instead of pretending
    // to drain it after the worker has already been asked to exit.
    size_t pending_packets = push_queue_.size();
    if (pending_packets > 0) {
        Log::warn("Discarding queued packets during stop: " + std::to_string(pending_packets));
        push_queue_.clear();
    }

    if (push_thread_.joinable()) {
        push_thread_.join();
    }

    rtmp_pusher_.disconnect();

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
    int consecutive_reconnect_failures = 0;  // 连续重连失败计数

    while (!stop_thread_) {
        if (push_queue_.pop(packet, 100)) {
            ErrorCode result = rtmp_pusher_.send_packet(packet);

            if (result != ErrorCode::SUCCESS) {
                Log::error("[PUSH] send_packet failed: error_code=" + std::to_string(static_cast<int>(result)) +
                           ", queue_size=" + std::to_string(push_queue_.size()));

                if (result == ErrorCode::NOT_CONNECTED) {
                    Log::error("[PUSH] NOT_CONNECTED: auto_reconnect=" + std::to_string(config_.auto_reconnect) +
                               ", reconnect_interval=" + std::to_string(config_.reconnect_interval_sec) + "s");
                    if (config_.auto_reconnect) {
                        auto now = std::chrono::steady_clock::now();
                        auto time_since_last_attempt = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_reconnect_attempt);

                        Log::info("[PUSH] Time since last reconnect attempt: " +
                                  std::to_string(time_since_last_attempt.count()) + "ms" +
                                  ", threshold=" + std::to_string(config_.reconnect_interval_sec * 1000) + "ms");

                        if (time_since_last_attempt.count() >= config_.reconnect_interval_sec * 1000) {
                            reconnect_attempts_++;
                            last_reconnect_attempt = now;
                            Log::info("[PUSH] Starting reconnect attempt #" + std::to_string(reconnect_attempts_));

                            if (reconnecting_callback_) {
                                reconnecting_callback_(reconnect_attempts_.load(),
                                                       config_.max_reconnect_attempts * 3);
                            }

                            ErrorCode reconnect_result = rtmp_pusher_.connect_and_write_header();
                            Log::info("[PUSH] Reconnect result: " + std::to_string(static_cast<int>(reconnect_result)));

                            if (reconnect_result == ErrorCode::SUCCESS) {
                                Log::info("[PUSH] Reconnected to RTMP server successfully, clearing queue");
                                if (reconnect_attempts_ > static_cast<int>(config_.max_reconnect_attempts) * 3) {
                                    Log::error("[PUSH] Total reconnect attempts (" +
                                        std::to_string(reconnect_attempts_.load()) +
                                        ") exceeded limit (" +
                                        std::to_string(config_.max_reconnect_attempts * 3) +
                                        "), giving up - likely invalid stream URL");
                                    set_state(StreamState::ERR);
                                    push_queue_.clear();
                                    stop_thread_ = true;
                                    break;
                                }
                                set_state(StreamState::PUSHING);
                                consecutive_reconnect_failures = 0;
                                push_queue_.clear();
                                if (reconnect_callback_) {
                                    reconnect_callback_();
                                    Log::info("[PUSH] Reconnect callback invoked (force IDR keyframe)");
                                }
                            } else {
                                Log::error("[PUSH] Reconnect attempt #" + std::to_string(reconnect_attempts_) +
                                           " failed (result=" + std::to_string(static_cast<int>(reconnect_result)) +
                                           "), consecutive_failures=" + std::to_string(consecutive_reconnect_failures + 1));
                                consecutive_reconnect_failures++;

                                if (consecutive_reconnect_failures >= config_.max_reconnect_attempts) {
                                    Log::error("[PUSH] Reconnect failed " + std::to_string(consecutive_reconnect_failures) +
                                              " times (max=" + std::to_string(config_.max_reconnect_attempts) +
                                              "), giving up and stopping streaming");
                                    set_state(StreamState::ERR);
                                    push_queue_.clear();
                                    stop_thread_ = true;
                                    break;
                                }
                            }
                        } else {
                            Log::info("[PUSH] Too soon since last reconnect attempt, waiting...");
                        }
                    } else {
                        Log::error("[PUSH] Connection lost and auto-reconnect disabled, stopping push thread");
                        set_state(StreamState::ERR);
                        push_queue_.clear();
                        break;
                    }
                } else if (result == ErrorCode::SEND_FAILED) {
                    Log::warn("[PUSH] Failed to send one packet (SEND_FAILED), continuing...");
                } else if (result == ErrorCode::INVALID_STATE) {
                    Log::error("[PUSH] INVALID_STATE in send_packet (stream/encoder not ready?), continuing...");
                }
            }
        }
    }

    Log::info("Push thread exited");
}

void StreamPusher::set_reconnect_callback(std::function<void()> callback) {
    reconnect_callback_ = std::move(callback);
}

void StreamPusher::set_reconnecting_callback(std::function<void(int, int)> callback) {
    reconnecting_callback_ = std::move(callback);
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
