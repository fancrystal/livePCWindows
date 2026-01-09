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
    
    // Check if stream is running
    if (state_ == StreamState::PUSHING || state_ == StreamState::CONNECTING) {
        Log::error("Cannot set config while stream is running");
        return ErrorCode::INVALID_STATE;
    }
    
    // Store configuration
    config_ = config;
    
    // Update queue max size
    push_queue_.set_max_size(config.max_queue_size);
    
    Log::info("Stream pusher config updated successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode StreamPusher::start() {
    Log::info("Starting stream pusher");
    
    // Check if already pushing
    if (state_ == StreamState::PUSHING || state_ == StreamState::CONNECTING) {
        Log::error("Stream is already pushing");
        return ErrorCode::ALREADY_RUNNING;
    }
    
    // Check if config is set
    if (config_.server_url.empty() || config_.stream_key.empty()) {
        Log::error("Stream config not set");
        return ErrorCode::INVALID_PARAM;
    }
    
    // Initialize RTMP pusher
    ErrorCode result = rtmp_pusher_.initialize(config_);
    if (result != ErrorCode::SUCCESS) {
        Log::error("Failed to initialize RTMP pusher");
        set_state(StreamState::ERR);
        return result;
    }
    
    // Set state to connecting
    set_state(StreamState::CONNECTING);
    
    // Connect to server
    result = rtmp_pusher_.connect();
    if (result != ErrorCode::SUCCESS) {
        Log::error("Failed to connect to RTMP server");
        set_state(StreamState::ERR);
        return result;
    }
    
    // Start push thread
    stop_thread_ = false;
    push_thread_ = std::thread(&StreamPusher::push_thread_func, this);
    
    // Set state to pushing
    set_state(StreamState::PUSHING);
    
    Log::info("Stream pusher started successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode StreamPusher::stop() {
    Log::info("Stopping stream pusher");
    
    // Check if already stopped
    if (state_ == StreamState::IDLE || state_ == StreamState::ERR) {
        return ErrorCode::SUCCESS;
    }
    
    // Set state to stopping
    set_state(StreamState::STOPPING);
    
    // Stop push thread
    stop_thread_ = true;
    if (push_thread_.joinable()) {
        push_thread_.join();
    }
    
    // Disconnect from server
    rtmp_pusher_.disconnect();
    
    // Clear queue
    push_queue_.clear();
    
    // Reset state
    set_state(StreamState::IDLE);
    
    Log::info("Stream pusher stopped successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode StreamPusher::push_packet(const MediaPacket& packet) {
    // Check if stream is pushing
    if (state_ != StreamState::PUSHING) {
        // Allow config packets even when not pushing
        if (!packet.is_config) {
            return ErrorCode::INVALID_STATE;
        }
    }
    
    // Push packet to queue
    if (!push_queue_.push(packet)) {
        Log::warn("Failed to push packet to queue");
        return ErrorCode::QUEUE_FULL;
    }
    
    return ErrorCode::SUCCESS;
}

void StreamPusher::push_thread_func() {
    Log::info("Push thread started");
    
    MediaPacket packet;
    
    while (!stop_thread_) {
        // Get next packet from queue
        if (push_queue_.pop(packet, 100)) {
            // Send packet
            ErrorCode result = rtmp_pusher_.send_packet(packet);
            
            if (result != ErrorCode::SUCCESS) {
                Log::error("Failed to send packet: " + std::to_string(static_cast<int>(result)));
                
                // Handle connection errors
                if (result == ErrorCode::NOT_CONNECTED) {
                    // Try to reconnect if enabled
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
                    // Continue sending other packets
                    Log::warn("Failed to send one packet, continuing...");
                }
            }
        }
        
        // Check if we need to stop
        if (stop_thread_) {
            break;
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
        
        // Wait before reconnect
        std::this_thread::sleep_for(std::chrono::seconds(config_.reconnect_interval_sec));
        
        // Try to connect
        ErrorCode result = rtmp_pusher_.connect();
        if (result == ErrorCode::SUCCESS) {
            Log::info("Reconnected to RTMP server successfully");
            set_state(StreamState::PUSHING);
            return ErrorCode::SUCCESS;
        }
        
        Log::error("Reconnect attempt failed");
        
        // Exponential backoff
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
    
    // Get RTMP pusher stats
    auto rtmp_stats = rtmp_pusher_.get_stats();
    stats.connected = rtmp_stats.connected;
    stats.audio_packets_sent = rtmp_stats.audio_packets_sent;
    stats.video_packets_sent = rtmp_stats.video_packets_sent;
    
    // Get queue stats
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