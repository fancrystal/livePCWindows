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

// CanvasConfig实现
namespace live_assistant {

CanvasConfig CanvasConfig::get_default() {
    return CanvasConfig(DisplayMode::LANDSCAPE_16_9);
}

CanvasConfig CanvasConfig::get_portrait() {
    return CanvasConfig(DisplayMode::PORTRAIT_9_16);
}

CanvasConfig::CanvasConfig(DisplayMode mode) : mode_(mode) {
    initialize_config();
}

void CanvasConfig::initialize_config() {
    switch (mode_) {
        case DisplayMode::LANDSCAPE_16_9:
            width_ = 1280;
            height_ = 720;
            aspect_ratio_ = 16.0 / 9.0;
            name_ = "Landscape 16:9 (1280x720)";
            break;

        case DisplayMode::PORTRAIT_9_16:
            width_ = 720;
            height_ = 1280;
            aspect_ratio_ = 9.0 / 16.0;
            name_ = "Portrait 9:16 (720x1280)";
            break;

        default:
            // 默认使用横屏16:9
            width_ = 1280;
            height_ = 720;
            aspect_ratio_ = 16.0 / 9.0;
            name_ = "Landscape 16:9 (1280x720)";
            break;
    }
}

bool CanvasConfig::is_size_compatible(int width, int height) const {
    if (width <= 0 || height <= 0) {
        return false;
    }

    double source_ratio = static_cast<double>(width) / height;
    double canvas_ratio = static_cast<double>(width_) / height_;

    // 允许一定的误差范围
    const double tolerance = 0.01;
    return std::abs(source_ratio - canvas_ratio) <= tolerance;
}

QRectF CanvasConfig::calculate_fit_rect(int source_width, int source_height,
                                       int target_width, int target_height) const {
    if (source_width <= 0 || source_height <= 0 ||
        target_width <= 0 || target_height <= 0) {
        return QRectF(0, 0, target_width, target_height);
    }

    double source_ratio = static_cast<double>(source_width) / source_height;
    double target_ratio = static_cast<double>(target_width) / target_height;

    QRectF result;

    if (source_ratio > target_ratio) {
        // 源图像更宽，需要裁剪宽度
        int new_width = static_cast<int>(target_height * source_ratio);
        int crop_x = (source_width - static_cast<int>(source_height * target_ratio)) / 2;

        result = QRectF(crop_x, 0,
                       static_cast<int>(source_height * target_ratio), source_height);
    } else {
        // 源图像更高，需要裁剪高度
        int new_height = static_cast<int>(target_width / source_ratio);
        int crop_y = (source_height - static_cast<int>(target_width / source_ratio)) / 2;

        result = QRectF(0, crop_y,
                       source_width, static_cast<int>(target_width / source_ratio));
    }

    return result;
}

} // namespace live_assistant