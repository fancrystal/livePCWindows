#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <atomic>
#include <mutex>

#include "common/error.h"
#include "common/media_clock.h"
#include "media_pipeline/source.h"
#include "media_pipeline/output.h"
#include "encoder/encoder.h"

namespace live_assistant {

// Forward declarations
class AudioEncoder;
class VideoEncoder;

// Pipeline component container
struct PipelineComponent {
    std::shared_ptr<Source> source;
    std::shared_ptr<AudioEncoder> audio_encoder;
    std::shared_ptr<VideoEncoder> video_encoder;
    std::shared_ptr<Output> output;
};

// Connection between components
struct Connection {
    std::string source_id;
    std::string encoder_id;
    std::string output_id;
};

// Pipeline configuration
struct PipelineConfig {
    std::vector<std::string> source_ids;
    std::vector<std::string> encoder_ids;
    std::vector<std::string> output_ids;
    std::vector<Connection> connections;
};

// Pipeline state enum
enum class PipelineState {
    IDLE,       // Initial state, no components running
    STARTING,   // Starting components
    RUNNING,    // All components running normally
    STOPPING,   // Stopping components
    ERROR       // Error occurred
};

// MediaPipeline class
class MediaPipeline {
public:
    MediaPipeline();
    ~MediaPipeline();

    // Initialize pipeline with configuration
    ErrorCode initialize(const PipelineConfig& config);

    // Shutdown pipeline completely
    ErrorCode shutdown();

    // Start pipeline
    ErrorCode start();

    // Stop pipeline
    ErrorCode stop();

    // Rebuild pipeline with new configuration
    ErrorCode rebuild(const PipelineConfig& new_config);

    // Add component to pipeline
    ErrorCode add_component(const std::string& id,
                           std::shared_ptr<Source> source,
                           std::shared_ptr<AudioEncoder> audio_encoder,
                           std::shared_ptr<VideoEncoder> video_encoder,
                           std::shared_ptr<Output> output);

    // Remove component from pipeline
    ErrorCode remove_component(const std::string& id);

    // Connect components in pipeline
    ErrorCode connect(const std::string& source_id,
                     const std::string& encoder_id,
                     const std::string& output_id);

    // Disconnect components in pipeline
    ErrorCode disconnect(const std::string& source_id,
                        const std::string& encoder_id,
                        const std::string& output_id);

    // Get pipeline state
    PipelineState get_state() const;

    // Check if pipeline is running
    bool is_running() const;

    // Check if pipeline is in error state
    bool is_in_error() const;

    // Get component by ID
    std::shared_ptr<PipelineComponent> get_component(const std::string& id) const;

    // Get all components
    std::unordered_map<std::string, std::shared_ptr<PipelineComponent>> get_all_components() const;

    // Get current configuration
    PipelineConfig get_config() const;

    // Reset pipeline state (only for error recovery)
    ErrorCode reset();

private:
    // Start components in order
    ErrorCode start_components();

    // Stop components in order
    ErrorCode stop_components();

    // Initialize components
    ErrorCode initialize_components();

    // Shutdown components
    ErrorCode shutdown_components();

    // Check if configuration is valid
    bool is_config_valid(const PipelineConfig& config) const;

    // Check if all components are initialized
    bool are_all_components_initialized() const;

    // Check if all components are running
    bool are_all_components_running() const;

    // Check if all components are stopped
    bool are_all_components_stopped() const;

    // Update pipeline state (thread-safe)
    void update_state(PipelineState new_state);

    // Current pipeline state
    std::atomic<PipelineState> state_;

    // Is pipeline rebuilding
    std::atomic<bool> is_rebuilding_;

    // Configuration
    PipelineConfig current_config_;

    // Components map (ID -> component)
    std::unordered_map<std::string, std::shared_ptr<PipelineComponent>> components_;

    // Connections
    std::vector<Connection> connections_;

    // Mutex for thread safety
    mutable std::mutex mutex_;
};

} // namespace live_assistant