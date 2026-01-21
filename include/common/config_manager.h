#pragma once

#include "common/error.h"
#include <any>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <QRectF>

namespace live_assistant {

/**
 * 画布配置类
 * 管理画布的显示比例、分辨率等配置
 */
class CanvasConfig {
public:
    /**
     * 画布显示模式
     */
    enum class DisplayMode {
        LANDSCAPE_16_9,    // 横屏16:9 (1920x1080)
        PORTRAIT_9_16      // 竖屏9:16 (1080x1920) - 预留
    };

    /**
     * 获取默认画布配置
     * @return 默认的横屏16:9配置
     */
    static CanvasConfig get_default();

    /**
     * 获取竖屏配置（预留）
     * @return 竖屏9:16配置
     */
    static CanvasConfig get_portrait();

    /**
     * 构造函数
     * @param mode 显示模式
     */
    explicit CanvasConfig(DisplayMode mode);

    // 获取配置属性
    DisplayMode get_display_mode() const { return mode_; }
    int get_width() const { return width_; }
    int get_height() const { return height_; }
    double get_aspect_ratio() const { return aspect_ratio_; }
    const std::string& get_name() const { return name_; }

    /**
     * 检查给定的尺寸是否适合当前画布
     * @param width 宽度
     * @param height 高度
     * @return 是否适合
     */
    bool is_size_compatible(int width, int height) const;

    /**
     * 计算适合画布的矩形（保持宽高比，可能裁剪）
     * @param source_width 源宽度
     * @param source_height 源高度
     * @param target_width 目标宽度
     * @param target_height 目标高度
     * @return 适合的矩形区域
     */
    QRectF calculate_fit_rect(int source_width, int source_height,
                             int target_width, int target_height) const;

private:
    DisplayMode mode_;
    int width_;
    int height_;
    double aspect_ratio_;
    std::string name_;

    void initialize_config();
};

/**
 * 配置管理器类
 * 负责加载、保存和管理应用程序配置
 */
class ConfigManager {
public:
    // 删除拷贝构造函数和赋值运算符，确保单例模式
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    /**
     * 获取ConfigManager单例实例
     * @return ConfigManager实例的引用
     */
    static ConfigManager& instance();

    /**
     * 从文件加载配置
     * @param filename 配置文件路径
     * @return 操作结果的错误码
     */
    ErrorCode load_from_file(const std::string& filename);

    /**
     * 保存配置到文件
     * @param filename 保存的文件路径
     * @return 操作结果的错误码
     */
    ErrorCode save_to_file(const std::string& filename) const;

    /**
     * 获取所有配置键
     * @return 包含所有配置键的向量
     */
    std::vector<std::string> get_all_keys() const;

private:
    // 私有构造函数，确保单例模式
    ConfigManager() = default;
    ~ConfigManager() = default;

    // 配置数据存储，使用any来存储不同类型的值
    std::unordered_map<std::string, std::any> config_map_;

    // 保护并发访问的互斥锁
    mutable std::mutex mutex_;
};

} // namespace live_assistant