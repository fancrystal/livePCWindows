#pragma once

#include "scene_manager/icapture_source.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace live_assistant {

class CaptureManager {
public:
    using CaptureSourcePtr = std::shared_ptr<ICaptureSource>;

    CaptureManager();
    ~CaptureManager();

    // Source management
    bool add_capture_source(const std::string& source_id, const CaptureConfig& config);
    bool remove_capture_source(const std::string& source_id);
    bool start_capture_source(const std::string& source_id);
    bool stop_capture_source(const std::string& source_id);
    bool has_capture_source(const std::string& source_id) const;

    // Configuration
    const CaptureConfig* get_source_config(const std::string& source_id) const;

    // Status
    bool is_source_running(const std::string& source_id) const;
    std::vector<std::string> get_all_source_ids() const;

    // Global operations
    bool start_all_sources();
    bool stop_all_sources();
    void remove_all_sources();

    // Statistics
    size_t get_source_count() const;
    size_t get_running_source_count() const;

    // Get source pointer (for signal-slot connections)
    CaptureSourcePtr get_source(const std::string& source_id) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, CaptureSourcePtr> sources_;

    // Helper methods
    CaptureSourcePtr get_source_locked(const std::string& source_id) const;
};

} // namespace live_assistant