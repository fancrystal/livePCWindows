#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

#include <QJsonObject>
#include <QJsonArray>

#include "common/error.h"
#include "media_pipeline/source.h"

namespace live_assistant {

// 前向声明
class Transform;

class Transform {
public:
    Transform() = default;
    Transform(int x, int y, int width, int height, float rotation = 0.0f, float opacity = 1.0f, bool mirror = false);
    
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    float rotation = 0.0f;
    float opacity = 1.0f;
    bool mirror = false;
};

// SceneItem表示Scene中的Source实例
class SceneItem {
public:
    SceneItem(std::shared_ptr<Source> source, const Transform& transform = Transform());
    ~SceneItem() = default;

    std::shared_ptr<Source> get_source() const;
    std::string get_source_id() const;

    // 设置/获取设备ID（用于序列化重建采集源）
    void set_device_id(const std::string& device_id);
    std::string get_device_id() const;

    // 设置/获取显示名称（友好的名称，如"USB Camera HD"）
    void set_display_name(const std::string& display_name);
    std::string get_display_name() const;

    // 设置/获取源参数（分辨率、帧率等）
    void set_source_params(const std::unordered_map<std::string, std::string>& params);
    std::unordered_map<std::string, std::string> get_source_params() const;

    void set_transform(const Transform& transform);
    Transform get_transform() const;

    void set_visible(bool visible);
    bool is_visible() const;

    void set_order(int order);
    int get_order() const;

private:
    std::shared_ptr<Source> source_;
    Transform transform_;
    bool visible_ = true;
    int order_ = 0;
    std::string device_id_;  // 设备原始ID（如摄像头的设备路径）
    std::string display_name_;  // 显示名称（友好的名称）
    std::unordered_map<std::string, std::string> source_params_;  // 源参数（分辨率、帧率等）
};

// Scene类表示包含多个SceneItem的场景
class Scene {
public:
    Scene(const std::string& name);
    ~Scene() = default;

    std::string get_name() const;

    // 设置场景名称（重命名）
    ErrorCode set_name(const std::string& name);

    // 序列化场景为JSON
    QJsonObject serialize() const;

    // 从JSON反序列化场景项
    static std::shared_ptr<SceneItem> deserialize_item(const QJsonObject& obj);
    
    // SceneItem管理
    std::shared_ptr<SceneItem> add_source(std::shared_ptr<Source> source);
    ErrorCode remove_scene_item(std::shared_ptr<SceneItem> item);
    ErrorCode remove_scene_item_by_source_id(const std::string& source_id);
    std::vector<std::shared_ptr<SceneItem>> get_all_scene_items() const;
    
    // 通过源ID获取SceneItem（返回第一个匹配项）
    std::shared_ptr<SceneItem> get_scene_item_by_source_id(const std::string& source_id) const;
    
    // 获取使用特定Source的所有SceneItem
    std::vector<std::shared_ptr<SceneItem>> get_scene_items_by_source_id(const std::string& source_id) const;
    
    // SceneItem变换
    ErrorCode set_transform(std::shared_ptr<SceneItem> item, const Transform& transform);
    
    // 源管理（便捷的直接方法）
    ErrorCode remove_source(const std::string& source_id);
    
public:
    // Layer (z-order) helpers
    ErrorCode move_scene_item_up(std::shared_ptr<SceneItem> item);
    ErrorCode move_scene_item_down(std::shared_ptr<SceneItem> item);
    ErrorCode set_scene_item_order(std::shared_ptr<SceneItem> item, int new_order);

public:
    void normalize_orders();

private:
    std::string name_;
    std::vector<std::shared_ptr<SceneItem>> scene_items_;
    int next_order_ = 0;
};

class SceneManager {
public:
    SceneManager();
    ~SceneManager() = default;
    
    ErrorCode create_scene(const std::string& name);
    ErrorCode remove_scene(const std::string& name);
    ErrorCode set_current_scene(const std::string& name);
    
    std::shared_ptr<Scene> get_current_scene() const;
    std::vector<std::string> get_scene_names() const;
    
    // SceneItem management
    std::shared_ptr<SceneItem> add_source_to_scene(const std::string& scene_name, std::shared_ptr<Source> source);
    // 带设备参数的重载版本（用于反序列化时重建采集源）
    std::shared_ptr<SceneItem> add_source_to_scene(const std::string& scene_name, std::shared_ptr<Source> source,
                                                   const std::string& device_id,
                                                   const std::unordered_map<std::string, std::string>& params);
    ErrorCode remove_source_from_scene(const std::string& scene_name, const std::string& source_id);
    ErrorCode remove_scene_item_from_scene(const std::string& scene_name, std::shared_ptr<SceneItem> item);
    
    // Get scene items for a specific scene
    std::vector<std::shared_ptr<SceneItem>> get_scene_items(const std::string& scene_name) const;

    // 序列化所有场景
    QJsonArray serialize() const;

    // 从JSON加载场景
    ErrorCode deserialize(const QJsonArray& scenes_array);

    // 清理所有场景中的采集源（在退出直播间时调用）
    void cleanup_all_sources();

    // 根据类型创建设备源（用于反序列化）
    static std::shared_ptr<Source> create_source_by_type(const std::string& type, const std::string& id, const std::string& device_id, const std::string& file_path);

private:
    std::vector<std::shared_ptr<Scene>> scenes_;
    size_t current_scene_index_ = 0;
};

} // namespace live_assistant
