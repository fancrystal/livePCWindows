#pragma once

#include "common/error.h"
#include <any>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace live_assistant {

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