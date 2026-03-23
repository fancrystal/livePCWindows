#include "scene_manager/capture_manager_iface.h"
#include "common/log.h"

namespace live_assistant {

CaptureManagerIface::~CaptureManagerIface() {
    // stop and clear all sources
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& kv : sources_) {
        if (kv.second) {
            kv.second->stop();
            kv.second->shutdown();
        }
    }
    sources_.clear();
}

bool CaptureManagerIface::add_source(const std::string& source_id, std::shared_ptr<ICaptureSource> source) {
    if (!source) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    if (sources_.count(source_id)) {
        LOG_WARNING("CaptureManagerIface: source exists: " + source_id);
        return false;
    }
    // CaptureFactory::create_capture_source() 已经调用过 initialize()，这里不再重复调用
    // if (!source->initialize()) {
    //     LOG_ERROR("CaptureManagerIface: failed to initialize source: " + source_id);
    //     return false;
    // }
    sources_.emplace(source_id, source);
    LOG_INFO("CaptureManagerIface: added source: " + source_id);
    return true;
}

bool CaptureManagerIface::remove_source(const std::string& source_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = sources_.find(source_id);
    if (it == sources_.end()) {
        LOG_WARNING("CaptureManagerIface: source not found for removal: " + source_id);
        return false;
    }
    LOG_INFO("CaptureManagerIface: removing source: " + source_id);
    LOG_INFO("CaptureManagerIface: calling stop() for: " + source_id);
    it->second->stop();
    LOG_INFO("CaptureManagerIface: calling shutdown() for: " + source_id);
    it->second->shutdown();
    sources_.erase(it);
    LOG_INFO("CaptureManagerIface: removed source: " + source_id);
    return true;
}

bool CaptureManagerIface::start_source(const std::string& source_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = sources_.find(source_id);
    if (it == sources_.end()) return false;
    return it->second->start();
}

bool CaptureManagerIface::stop_source(const std::string& source_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = sources_.find(source_id);
    if (it == sources_.end()) return false;
    return it->second->stop();
}

bool CaptureManagerIface::has_source(const std::string& source_id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return sources_.count(source_id) > 0;
}

std::vector<std::string> CaptureManagerIface::get_all_source_ids() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<std::string> ids;
    ids.reserve(sources_.size());
    for (const auto& kv : sources_) {
        ids.push_back(kv.first);
    }
    return ids;
}

} // namespace live_assistant

