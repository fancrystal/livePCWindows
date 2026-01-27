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
    
    stop_thread_ = true;
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

    Log::info("push_packet called, attempting to enqueue packet");
    if (!push_queue_.push(std::move(packet))) {
        Log::warn("Failed to push packet to queue, queue is full");
        return ErrorCode::QUEUE_FULL;
    }

    Log::info("Packet enqueued successfully");
    return ErrorCode::SUCCESS;
}

void StreamPusher::push_thread_func() {
    Log::info("Push thread started");
    
    EncodedPacketPtr packet;
    
    while (!stop_thread_) {
        if (push_queue_.pop(packet, 100)) {
            Log::info("Popped packet from queue, sending...");
            ErrorCode result = rtmp_pusher_.send_packet(packet);
            Log::info("send_packet returned: " + std::to_string(static_cast<int>(result)));

            if (result != ErrorCode::SUCCESS) {
                Log::error("Failed to send packet: " + std::to_string(static_cast<int>(result)));
                
                if (result == ErrorCode::NOT_CONNECTED) {
                    if (config_.auto_reconnect) {
                        ErrorCode reconnect_result = try_reconnect();
                        if (reconnect_result != ErrorCode::SUCCESS) {
                            Log::error("Failed to reconnect after multiple attempts");
                            set_state(StreamState::ERR);
                            break;
                        }
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
    }
    
    Log::info("Push thread exited");
}

ErrorCode StreamPusher::try_reconnect() {
    Log::info("Attempting to reconnect to RTMP server");
    
    int max_attempts = config_.max_reconnect_attempts;
    int attempt = 0;
    
    while (attempt < max_attempts && !stop_thread_) {
        attempt++;
        reconnect_attempts_++;
        
        Log::info("Reconnect attempt " + std::to_string(attempt) + "/" + std::to_string(max_attempts));
        
        std::this_thread::sleep_for(std::chrono::seconds(config_.reconnect_interval_sec));
        
        ErrorCode result = rtmp_pusher_.connect_and_write_header();
        if (result == ErrorCode::SUCCESS) {
            Log::info("Reconnected to RTMP server successfully");
            set_state(StreamState::PUSHING);
            return ErrorCode::SUCCESS;
        }
        
        Log::error("Reconnect attempt failed");
        std::this_thread::sleep_for(std::chrono::seconds(config_.reconnect_interval_sec * attempt));
    }
    
    Log::error("All reconnect attempts failed");
    return ErrorCode::CONNECT_FAILED;
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
