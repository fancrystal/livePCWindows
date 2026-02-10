#include "media_pipeline/media_pipeline.h"
#include "common/log.h"
#include "common/error.h"
#include "encoder/encoder_factory.h"

namespace live_assistant {

MediaPipeline::MediaPipeline() : 
    state_(PipelineState::IDLE),
    is_rebuilding_(false) {
    Log::info("MediaPipeline constructor");
}

MediaPipeline::~MediaPipeline() {
    shutdown();
    Log::info("MediaPipeline destructor");
}

ErrorCode MediaPipeline::initialize(const PipelineConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Log::info("Initializing MediaPipeline");
    
    // Check if already initialized
    if (state_ != PipelineState::IDLE) {
        Log::error("Pipeline already initialized");
        return ErrorCode::ALREADY_INITIALIZED;
    }
    
    // Validate configuration
    if (!is_config_valid(config)) {
        Log::error("Invalid pipeline configuration");
        return ErrorCode::INVALID_PARAM;
    }
    
    // Store configuration
    current_config_ = config;
    
    Log::info("MediaPipeline initialized successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode MediaPipeline::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Log::info("Shutting down MediaPipeline");
    
    // Stop components if running
    if (state_ == PipelineState::RUNNING) {
        stop_components();
    }
    
    // Shutdown components
    ErrorCode result = shutdown_components();
    if (result != ErrorCode::SUCCESS) {
        Log::error("Failed to shutdown components");
        return result;
    }
    
    // Clear components and connections
    components_.clear();
    connections_.clear();
    
    // Reset state
    update_state(PipelineState::IDLE);
    is_rebuilding_ = false;
    
    Log::info("MediaPipeline shutdown successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode MediaPipeline::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Log::info("Starting MediaPipeline");
    
    // Check if pipeline is in valid state for starting
    if (state_.load() != PipelineState::IDLE) {
        Log::error("Pipeline is not in IDLE state, current state: " + std::to_string(static_cast<int>(state_.load())));
        return ErrorCode::INVALID_STATE;
    }
    
    // Update state to STARTING
    update_state(PipelineState::STARTING);
    
    // Initialize components
    ErrorCode init_result = initialize_components();
    if (init_result != ErrorCode::SUCCESS) {
        Log::error("Failed to initialize components");
        update_state(PipelineState::ERROR);
        return init_result;
    }
    
    // Start components
    ErrorCode start_result = start_components();
    if (start_result != ErrorCode::SUCCESS) {
        Log::error("Failed to start components");
        stop_components();
        update_state(PipelineState::ERROR);
        return start_result;
    }
    
    // Update state to RUNNING
    update_state(PipelineState::RUNNING);
    
    Log::info("MediaPipeline started successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode MediaPipeline::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Log::info("Stopping MediaPipeline");
    
    // Check if pipeline is in valid state for stopping
    if (state_.load() != PipelineState::RUNNING) {
        Log::error("Pipeline is not in RUNNING state, current state: " + std::to_string(static_cast<int>(state_.load())));
        return ErrorCode::INVALID_STATE;
    }
    
    // Update state to STOPPING
    update_state(PipelineState::STOPPING);
    
    // Stop components
    ErrorCode result = stop_components();
    if (result != ErrorCode::SUCCESS) {
        Log::error("Failed to stop components");
        update_state(PipelineState::ERROR);
        return result;
    }
    
    // Update state to IDLE
    update_state(PipelineState::IDLE);
    
    Log::info("MediaPipeline stopped successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode MediaPipeline::rebuild(const PipelineConfig& new_config) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Log::info("Rebuilding MediaPipeline");
    
    // Set rebuilding flag
    is_rebuilding_ = true;
    
    // Stop if running
    if (state_ == PipelineState::RUNNING) {
        update_state(PipelineState::STOPPING);
        stop_components();
        update_state(PipelineState::IDLE);
    }
    
    // Validate new configuration
    if (!is_config_valid(new_config)) {
        Log::error("Invalid new pipeline configuration");
        is_rebuilding_ = false;
        return ErrorCode::INVALID_PARAM;
    }
    
    // Update configuration
    current_config_ = new_config;
    
    // Shutdown current components
    shutdown_components();
    
    // Clear components and connections
    components_.clear();
    connections_.clear();
    
    // Initialize and start with new configuration
    ErrorCode init_result = initialize_components();
    if (init_result != ErrorCode::SUCCESS) {
        Log::error("Failed to initialize components during rebuild");
        is_rebuilding_ = false;
        update_state(PipelineState::ERROR);
        return init_result;
    }
    
    ErrorCode start_result = start_components();
    if (start_result != ErrorCode::SUCCESS) {
        Log::error("Failed to start components during rebuild");
        stop_components();
        is_rebuilding_ = false;
        update_state(PipelineState::ERROR);
        return start_result;
    }
    
    // Update state to RUNNING
    update_state(PipelineState::RUNNING);
    is_rebuilding_ = false;
    
    Log::info("MediaPipeline rebuilt successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode MediaPipeline::add_component(const std::string& id,
                                     std::shared_ptr<Source> source,
                                     std::shared_ptr<AudioEncoder> audio_encoder,
                                     std::shared_ptr<VideoEncoder> video_encoder,
                                     std::shared_ptr<Output> output) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Log::info("Adding component: " + id);
    
    // Check if already exists
    if (components_.find(id) != components_.end()) {
        Log::error("Component already exists: " + id);
        return ErrorCode::ALREADY_EXISTS;
    }
    
    // Create component
    auto component = std::make_shared<PipelineComponent>();
    component->source = source;
    component->audio_encoder = audio_encoder;
    component->video_encoder = video_encoder;
    component->output = output;
    
    // Add to components map
    components_[id] = component;
    
    Log::info("Component added successfully: " + id);
    return ErrorCode::SUCCESS;
}

ErrorCode MediaPipeline::remove_component(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Log::info("Removing component: " + id);
    
    // Check if exists
    auto it = components_.find(id);
    if (it == components_.end()) {
        Log::error("Component not found: " + id);
        return ErrorCode::NOT_FOUND;
    }
    
    // Stop component if running
    if (state_ == PipelineState::RUNNING) {
        // Get component
        auto component = it->second;
        
        // Stop output
        if (component->output) {
            component->output->stop();
        }
        
        // Stop encoders
        if (component->audio_encoder) {
            component->audio_encoder->shutdown();
        }
        if (component->video_encoder) {
            component->video_encoder->shutdown();
        }
        
        // Stop source
        if (component->source) {
            component->source->stop();
        }
    }
    
    // Remove component
    components_.erase(it);
    
    // Remove related connections
    connections_.erase(
        std::remove_if(connections_.begin(), connections_.end(),
                      [id](const Connection& conn) {
                          return conn.source_id == id || 
                                 conn.encoder_id == id || 
                                 conn.output_id == id;
                      }),
        connections_.end());
    
    Log::info("Component removed successfully: " + id);
    return ErrorCode::SUCCESS;
}

ErrorCode MediaPipeline::connect(const std::string& source_id,
                                const std::string& encoder_id,
                                const std::string& output_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Log::info("Connecting components: " + source_id + " -> " + encoder_id + " -> " + output_id);
    
    // Check if components exist
    if (components_.find(source_id) == components_.end()) {
        Log::error("Source not found: " + source_id);
        return ErrorCode::NOT_FOUND;
    }
    
    if (components_.find(encoder_id) == components_.end()) {
        Log::error("Encoder not found: " + encoder_id);
        return ErrorCode::NOT_FOUND;
    }
    
    if (components_.find(output_id) == components_.end()) {
        Log::error("Output not found: " + output_id);
        return ErrorCode::NOT_FOUND;
    }
    
    // Create connection
    Connection conn;
    conn.source_id = source_id;
    conn.encoder_id = encoder_id;
    conn.output_id = output_id;
    
    // Add to connections
    connections_.push_back(conn);
    
    Log::info("Components connected successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode MediaPipeline::disconnect(const std::string& source_id,
                                   const std::string& encoder_id,
                                   const std::string& output_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Log::info("Disconnecting components: " + source_id + " -> " + encoder_id + " -> " + output_id);
    
    // Find and remove connection
    auto it = std::remove_if(connections_.begin(), connections_.end(),
                           [source_id, encoder_id, output_id](const Connection& conn) {
                               return conn.source_id == source_id && 
                                      conn.encoder_id == encoder_id && 
                                      conn.output_id == output_id;
                           });
    
    if (it == connections_.end()) {
        Log::error("Connection not found");
        return ErrorCode::NOT_FOUND;
    }
    
    connections_.erase(it, connections_.end());
    
    Log::info("Components disconnected successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode MediaPipeline::start_components() {
    Log::info("Starting pipeline components");
    
    // Start order: Source -> Encoder -> Output
    
    // Start sources first
    for (const auto& component_pair : components_) {
        auto& component = component_pair.second;
        if (component->source) {
            bool result = component->source->start();
            if (!result) {
                Log::error("Failed to start source: " + component_pair.first);
                return ErrorCode::FAILURE;
            }
        }
    }
    
    // Start encoders next
    for (const auto& component_pair : components_) {
        auto& component = component_pair.second;
        // Encoders are already initialized during pipeline initialization
    }
    
    // Start outputs last
    for (const auto& component_pair : components_) {
        auto& component = component_pair.second;
        if (component->output) {
            ErrorCode result = component->output->start();
            if (result != ErrorCode::SUCCESS) {
                Log::error("Failed to start output: " + component_pair.first);
                return ErrorCode::FAILURE;
            }
        }
    }

    Log::info("All pipeline components started successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode MediaPipeline::stop_components() {
    Log::info("Stopping pipeline components");

    // Stop order: Output -> Encoder -> Source

    // Stop outputs first
    for (const auto& component_pair : components_) {
        auto& component = component_pair.second;
        if (component->output) {
            ErrorCode result = component->output->stop();
            if (result != ErrorCode::SUCCESS) {
                Log::warn("Failed to stop output: " + component_pair.first + ", continuing...");
            }
        }
    }

    // Stop encoders next
    for (const auto& component_pair : components_) {
        auto& component = component_pair.second;
        if (component->audio_encoder) {
            ErrorCode result = component->audio_encoder->shutdown();
            if (result != ErrorCode::SUCCESS) {
                Log::warn("Failed to shutdown audio encoder: " + component_pair.first + ", continuing...");
            }
        }
        if (component->video_encoder) {
            ErrorCode result = component->video_encoder->shutdown();
            if (result != ErrorCode::SUCCESS) {
                Log::warn("Failed to shutdown video encoder: " + component_pair.first + ", continuing...");
            }
        }
    }

    // Stop sources last
    for (const auto& component_pair : components_) {
        auto& component = component_pair.second;
        if (component->source) {
            bool result = component->source->stop();
            if (!result) {
                Log::warn("Failed to stop source: " + component_pair.first + ", continuing...");
            }
        }
    }

    Log::info("All pipeline components stopped");
    return ErrorCode::SUCCESS;
}

ErrorCode MediaPipeline::initialize_components() {
    Log::info("Initializing pipeline components");

    // Initialize all components
    for (const auto& component_pair : components_) {
        auto& component = component_pair.second;

        if (component->source) {
            bool result = component->source->initialize();
            if (!result) {
                Log::error("Failed to initialize source: " + component_pair.first);
                return ErrorCode::FAILURE;
            }
        }

        if (component->output) {
            ErrorCode result = component->output->initialize();
            if (result != ErrorCode::SUCCESS) {
                Log::error("Failed to initialize output: " + component_pair.first);
                return ErrorCode::FAILURE;
            }
        }
    }

    Log::info("All pipeline components initialized successfully");
    return ErrorCode::SUCCESS;
}

ErrorCode MediaPipeline::shutdown_components() {
    Log::info("Shutting down pipeline components");

    // Shutdown all components
    for (const auto& component_pair : components_) {
        auto& component = component_pair.second;

        if (component->source) {
            bool result = component->source->shutdown();
            if (!result) {
                Log::warn("Failed to shutdown source: " + component_pair.first + ", continuing...");
            }
        }

        if (component->audio_encoder) {
            ErrorCode result = component->audio_encoder->shutdown();
            if (result != ErrorCode::SUCCESS) {
                Log::warn("Failed to shutdown audio encoder: " + component_pair.first + ", continuing...");
            }
        }

        if (component->video_encoder) {
            ErrorCode result = component->video_encoder->shutdown();
            if (result != ErrorCode::SUCCESS) {
                Log::warn("Failed to shutdown video encoder: " + component_pair.first + ", continuing...");
            }
        }

        if (component->output) {
            ErrorCode result = component->output->shutdown();
            if (result != ErrorCode::SUCCESS) {
                Log::warn("Failed to shutdown output: " + component_pair.first + ", continuing...");
            }
        }
    }

    Log::info("All pipeline components shutdown");
    return ErrorCode::SUCCESS;
}

bool MediaPipeline::is_config_valid(const PipelineConfig& config) const {
    // Basic validation
    if (config.source_ids.empty() || config.output_ids.empty()) {
        return false;
    }
    
    // Check if connections are valid
    for (const auto& conn : config.connections) {
        bool source_exists = std::find(config.source_ids.begin(), config.source_ids.end(), conn.source_id) != config.source_ids.end();
        bool encoder_exists = std::find(config.encoder_ids.begin(), config.encoder_ids.end(), conn.encoder_id) != config.encoder_ids.end();
        bool output_exists = std::find(config.output_ids.begin(), config.output_ids.end(), conn.output_id) != config.output_ids.end();
        
        if (!source_exists || !encoder_exists || !output_exists) {
            return false;
        }
    }
    
    return true;
}

bool MediaPipeline::are_all_components_initialized() const {
    for (const auto& component_pair : components_) {
        const auto& component = component_pair.second;
        
        // Check source
        if (component->source && !component->source->is_running()) {
            return false;
        }
        
        // Check output
        if (component->output && !component->output->is_running()) {
            return false;
        }
    }
    
    return true;
}

bool MediaPipeline::are_all_components_running() const {
    for (const auto& component_pair : components_) {
        const auto& component = component_pair.second;
        
        // Check source
        if (component->source && !component->source->is_running()) {
            return false;
        }
        
        // Check output
        if (component->output && !component->output->is_running()) {
            return false;
        }
    }
    
    return true;
}

bool MediaPipeline::are_all_components_stopped() const {
    for (const auto& component_pair : components_) {
        const auto& component = component_pair.second;
        
        // Check source
        if (component->source && component->source->is_running()) {
            return false;
        }
        
        // Check output
        if (component->output && component->output->is_running()) {
            return false;
        }
    }
    
    return true;
}

void MediaPipeline::update_state(PipelineState new_state) {
    PipelineState old_state = state_.exchange(new_state);
    Log::info("Pipeline state changed: " +
             std::to_string(static_cast<int>(old_state)) + " -> " +
             std::to_string(static_cast<int>(new_state)));
}

PipelineState MediaPipeline::get_state() const {
    return state_.load();
}

bool MediaPipeline::is_running() const {
    return state_.load() == PipelineState::RUNNING;
}

bool MediaPipeline::is_in_error() const {
    return state_.load() == PipelineState::ERROR;
}

std::shared_ptr<PipelineComponent> MediaPipeline::get_component(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = components_.find(id);
    if (it != components_.end()) {
        return it->second;
    }
    return nullptr;
}

std::unordered_map<std::string, std::shared_ptr<PipelineComponent>> MediaPipeline::get_all_components() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return components_;
}

PipelineConfig MediaPipeline::get_config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_config_;
}

ErrorCode MediaPipeline::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Log::info("Resetting MediaPipeline state");
    
    // Only allow reset from ERROR state
    if (state_ != PipelineState::ERROR) {
        Log::error("Cannot reset pipeline from non-ERROR state");
        return ErrorCode::INVALID_STATE;
    }
    
    // Update state to IDLE
    update_state(PipelineState::IDLE);
    
    Log::info("MediaPipeline state reset to IDLE");
    return ErrorCode::SUCCESS;
}

} // namespace live_assistant