#include "scene_manager/capture_manager.h"
#include "common/log.h"
#include <algorithm>

namespace live_assistant {

CaptureManager::CaptureManager() {
    LOG_INFO("CaptureManager created");
}

CaptureManager::~CaptureManager() {
    remove_all_sources();
    LOG_INFO("CaptureManager destroyed");
}

bool CaptureManager::add_capture_source(const std::string& source_id, const CaptureSource::CaptureConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if source already exists
    if (sources_.find(source_id) != sources_.end()) {
        LOG_WARNING("Capture source already exists: " + source_id);
        return false;
    }

    LOG_INFO("Adding capture source: " + source_id + " (target: " + config.target_id + ")");

    try {
        // Create new capture source
        auto source = std::make_shared<CaptureSource>(config);

        // Initialize the source
        if (!source->initialize()) {
            LOG_ERROR("Failed to initialize capture source: " + source_id);
            return false;
        }

        // Add to map
        sources_[source_id] = source;

        LOG_INFO("Capture source added successfully: " + source_id);
        return true;

    } catch (const std::exception& ex) {
        LOG_ERROR("Exception adding capture source " + source_id + ": " + ex.what());
        return false;
    }
}

bool CaptureManager::remove_capture_source(const std::string& source_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sources_.find(source_id);
    if (it == sources_.end()) {
        LOG_WARNING("Capture source not found: " + source_id);
        return false;
    }

    LOG_INFO("Removing capture source: " + source_id);

    try {
        // Stop the source if running
        if (it->second->is_running()) {
            it->second->stop();
        }

        // Shutdown the source
        it->second->shutdown();

        // Remove from map
        sources_.erase(it);

        LOG_INFO("Capture source removed successfully: " + source_id);
        return true;

    } catch (const std::exception& ex) {
        LOG_ERROR("Exception removing capture source " + source_id + ": " + ex.what());
        return false;
    }
}

bool CaptureManager::start_capture_source(const std::string& source_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto source = get_source_locked(source_id);
    if (!source) {
        return false;
    }

    if (source->is_running()) {
        LOG_WARNING("Capture source already running: " + source_id);
        return true;
    }

    LOG_INFO("Starting capture source: " + source_id);

    if (source->start()) {
        LOG_INFO("Capture source started successfully: " + source_id);
        return true;
    } else {
        LOG_ERROR("Failed to start capture source: " + source_id);
        return false;
    }
}

bool CaptureManager::stop_capture_source(const std::string& source_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto source = get_source_locked(source_id);
    if (!source) {
        return false;
    }

    if (!source->is_running()) {
        LOG_WARNING("Capture source not running: " + source_id);
        return true;
    }

    LOG_INFO("Stopping capture source: " + source_id);

    if (source->stop()) {
        LOG_INFO("Capture source stopped successfully: " + source_id);
        return true;
    } else {
        LOG_ERROR("Failed to stop capture source: " + source_id);
        return false;
    }
}

bool CaptureManager::has_capture_source(const std::string& source_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sources_.find(source_id) != sources_.end();
}

void CaptureManager::set_frame_callback(const std::string& source_id, FrameCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto source = get_source_locked(source_id);
    if (source) {
        source->set_frame_callback(callback);
        LOG_INFO("Frame callback set for source: " + source_id);
    }
}

const CaptureSource::CaptureConfig* CaptureManager::get_source_config(const std::string& source_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto source = get_source_locked(source_id);
    if (source) {
        return &source->get_config();
    }
    return nullptr;
}

bool CaptureManager::is_source_running(const std::string& source_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto source = get_source_locked(source_id);
    return source && source->is_running();
}

std::vector<std::string> CaptureManager::get_all_source_ids() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> ids;
    ids.reserve(sources_.size());

    for (const auto& pair : sources_) {
        ids.push_back(pair.first);
    }

    return ids;
}

bool CaptureManager::start_all_sources() {
    std::lock_guard<std::mutex> lock(mutex_);

    LOG_INFO("Starting all capture sources");

    bool all_success = true;
    for (const auto& pair : sources_) {
        if (!pair.second->is_running()) {
            if (!pair.second->start()) {
                LOG_ERROR("Failed to start capture source: " + pair.first);
                all_success = false;
            }
        }
    }

    if (all_success) {
        LOG_INFO("All capture sources started successfully");
    } else {
        LOG_WARNING("Some capture sources failed to start");
    }

    return all_success;
}

bool CaptureManager::stop_all_sources() {
    std::lock_guard<std::mutex> lock(mutex_);

    LOG_INFO("Stopping all capture sources");

    bool all_success = true;
    for (const auto& pair : sources_) {
        if (pair.second->is_running()) {
            if (!pair.second->stop()) {
                LOG_ERROR("Failed to stop capture source: " + pair.first);
                all_success = false;
            }
        }
    }

    if (all_success) {
        LOG_INFO("All capture sources stopped successfully");
    } else {
        LOG_WARNING("Some capture sources failed to stop");
    }

    return all_success;
}

void CaptureManager::remove_all_sources() {
    std::lock_guard<std::mutex> lock(mutex_);

    LOG_INFO("Removing all capture sources");

    // Create a copy of source IDs to avoid iterator invalidation
    auto source_ids = get_all_source_ids();

    for (const auto& source_id : source_ids) {
        remove_capture_source(source_id);
    }

    LOG_INFO("All capture sources removed");
}

size_t CaptureManager::get_source_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sources_.size();
}

size_t CaptureManager::get_running_source_count() const {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t running_count = 0;
    for (const auto& pair : sources_) {
        if (pair.second->is_running()) {
            ++running_count;
        }
    }

    return running_count;
}

CaptureManager::CaptureSourcePtr CaptureManager::get_source_locked(const std::string& source_id) const {
    auto it = sources_.find(source_id);
    if (it != sources_.end()) {
        return it->second;
    }
    return nullptr;
}

} // namespace live_assistant