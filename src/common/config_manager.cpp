#include "common/config_manager.h"
#include "common/log.h"
#include <any>
#include <fstream>
#include <sstream>

namespace live_assistant {

// ConfigManager实现
ConfigManager& ConfigManager::instance() {
    static ConfigManager instance;
    return instance;
}

ErrorCode ConfigManager::load_from_file(const std::string& filename) {
    try {
        std::ifstream file(filename);
        if (!file.is_open()) {
            LOG_WARNING("Failed to open config file: " + filename);
            return ErrorCode::NOT_FOUND;
        }

        // 这里可以实现JSON解析逻辑
        // 暂时只记录日志
        LOG_INFO("Loading config from file: " + filename);
        return ErrorCode::SUCCESS;

    } catch (const std::exception& e) {
        LOG_ERROR("Exception while loading config: " + std::string(e.what()));
        return ErrorCode::FAILURE;
    }
}

ErrorCode ConfigManager::save_to_file(const std::string& filename) const {
    try {
        std::ofstream file(filename);
        if (!file.is_open()) {
            LOG_ERROR("Failed to open config file for writing: " + filename);
            return ErrorCode::FAILURE;
        }

        // 这里可以实现JSON序列化逻辑
        // 暂时只记录日志
        LOG_INFO("Saving config to file: " + filename);
        return ErrorCode::SUCCESS;

    } catch (const std::exception& e) {
        LOG_ERROR("Exception while saving config: " + std::string(e.what()));
        return ErrorCode::FAILURE;
    }
}

std::vector<std::string> ConfigManager::get_all_keys() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> keys;
    keys.reserve(config_map_.size());
    for (const auto& pair : config_map_) {
        keys.push_back(pair.first);
    }
    return keys;
}

} // namespace live_assistant