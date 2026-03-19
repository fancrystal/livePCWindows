#pragma once

#include "scene_manager/icapture_source.h"
#include <unordered_map>
#include <memory>
#include <mutex>

namespace live_assistant {

class CaptureManagerIface {
public:
    CaptureManagerIface() = default;
    ~CaptureManagerIface();

    bool add_source(const std::string& source_id, std::shared_ptr<ICaptureSource> source);
    bool remove_source(const std::string& source_id);
    bool start_source(const std::string& source_id);
    bool stop_source(const std::string& source_id);
    bool has_source(const std::string& source_id) const;
    std::vector<std::string> get_all_source_ids() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<ICaptureSource>> sources_;
};

} // namespace live_assistant

