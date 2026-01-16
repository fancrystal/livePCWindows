#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

#include "common/error.h"
#include "media_pipeline/source.h"

namespace live_assistant {

// 前向声明
class Transform;

class Transform {
public:
    Transform() = default;
    Transform(int x, int y, int width, int height, float rotation = 0.0f, float opacity = 1.0f);
    
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    float rotation = 0.0f;
    float opacity = 1.0f;
};

// SceneItem表示Scene中的Source实例
// OBS等价物：SceneItem
class SceneItem {
public:
    SceneItem(std::shared_ptr<Source> source, const Transform& transform = Transform());
    ~SceneItem() = default;
    
    std::shared_ptr<Source> get_source() const;
    std::string get_source_id() const;
    
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
};

// Scene类表示包含多个SceneItem的场景
// OBS等价物：Scene
class Scene {
public:
    Scene(const std::string& name);
    ~Scene() = default;
    
    std::string get_name() const;
    
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
    ErrorCode remove_source_from_scene(const std::string& scene_name, const std::string& source_id);
    ErrorCode remove_scene_item_from_scene(const std::string& scene_name, std::shared_ptr<SceneItem> item);
    
    // Get scene items for a specific scene
    std::vector<std::shared_ptr<SceneItem>> get_scene_items(const std::string& scene_name) const;
    
private:
    std::vector<std::shared_ptr<Scene>> scenes_;
    size_t current_scene_index_ = 0;
};

} // namespace live_assistant
