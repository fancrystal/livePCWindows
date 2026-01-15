#include "scene_manager/scene_manager.h"
#include "common/log.h"
#include "common/error.h"
#include <algorithm>

namespace live_assistant {

// Transform实现
Transform::Transform(int x, int y, int width, int height, float rotation, float opacity)
    : x(x), y(y), width(width), height(height), rotation(rotation), opacity(opacity) {
}



// SceneItem实现
SceneItem::SceneItem(std::shared_ptr<Source> source, const Transform& transform) 
    : source_(source), transform_(transform) {
    LOG_INFO("Created scene item for source: " + source->get_id());
}

std::shared_ptr<Source> SceneItem::get_source() const {
    return source_;
}

std::string SceneItem::get_source_id() const {
    return source_->get_id();
}

void SceneItem::set_transform(const Transform& transform) {
    transform_ = transform;
}

Transform SceneItem::get_transform() const {
    return transform_;
}

void SceneItem::set_visible(bool visible) {
    visible_ = visible;
}

bool SceneItem::is_visible() const {
    return visible_;
}

void SceneItem::set_order(int order) {
    order_ = order;
}

int SceneItem::get_order() const {
    return order_;
}

// Forward declaration helper
namespace {
    void sort_items_by_order(std::vector<std::shared_ptr<SceneItem>>& items) {
        std::sort(items.begin(), items.end(), [](const auto& a, const auto& b){
            return a->get_order() < b->get_order();
        });
    }
}

// Scene实现
Scene::Scene(const std::string& name) : name_(name) {
    LOG_INFO("Created scene: " + name);
}

std::string Scene::get_name() const {
    return name_;
}

std::shared_ptr<SceneItem> Scene::add_source(std::shared_ptr<Source> source) {
    // 创建默认变换的场景项，设置合理的默认大小和位置
    Transform default_transform;
    // 设置默认大小为640x360
    default_transform.width = 640;
    default_transform.height = 360;
    // 设置默认位置为居中
    default_transform.x = 100; // 暂时使用固定值，实际应该根据画布大小计算
    default_transform.y = 100;
    // 设置默认不透明度为1.0
    default_transform.opacity = 1.0f;
    
    auto item = std::make_shared<SceneItem>(source, default_transform);
    
    // 根据当前计数设置顺序（越大越靠上）
    item->set_order(next_order_++);

    // 追加到末尾
    scene_items_.push_back(item);
    
    LOG_INFO("Added source " + source->get_id() + " to scene " + name_);
    return item;
}

ErrorCode Scene::remove_scene_item(std::shared_ptr<SceneItem> item) {
    auto it = std::find(scene_items_.begin(), scene_items_.end(), item);
    if (it != scene_items_.end()) {
        // 为了避免循环引用，在移除之前清除SceneItem中的Source引用
        (*it)->set_transform(Transform()); // 重置变换
        (*it)->set_visible(false); // 隐藏
        scene_items_.erase(it);
        LOG_INFO("Removed scene item for source: " + item->get_source_id());
        return ErrorCode::SUCCESS;
    }
    LOG_WARNING("Scene item not found in scene: " + name_);
    return ErrorCode::FAILURE;
}

ErrorCode Scene::remove_scene_item_by_source_id(const std::string& source_id) {
    bool removed = false;
    auto it = scene_items_.begin();
    while (it != scene_items_.end()) {
        if ((*it)->get_source_id() == source_id) {
            // 为了避免循环引用，在移除之前清除SceneItem中的Source引用
            (*it)->set_transform(Transform()); // 重置变换
            (*it)->set_visible(false); // 隐藏
            it = scene_items_.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }

    if (removed) {
        LOG_INFO("Removed scene items for source: " + source_id);
        return ErrorCode::SUCCESS;
    }

    LOG_WARNING("No scene items found for source: " + source_id);
    return ErrorCode::FAILURE;
}

ErrorCode Scene::remove_source(const std::string& source_id) {
    return remove_scene_item_by_source_id(source_id);
}

std::vector<std::shared_ptr<SceneItem>> Scene::get_all_scene_items() const {
    // 按order升序返回，保证绘制顺序
    auto items = scene_items_;
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b){ return a->get_order() < b->get_order();});
    return items;
}

std::shared_ptr<SceneItem> Scene::get_scene_item_by_source_id(const std::string& source_id) const {
    for (const auto& item : scene_items_) {
        if (item->get_source_id() == source_id) {
            return item;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<SceneItem>> Scene::get_scene_items_by_source_id(const std::string& source_id) const {
    std::vector<std::shared_ptr<SceneItem>> result;
    for (const auto& item : scene_items_) {
        if (item->get_source_id() == source_id) {
            result.push_back(item);
        }
    }
    return result;
}

ErrorCode Scene::set_transform(std::shared_ptr<SceneItem> item, const Transform& transform) {
    // 检查场景项是否属于此场景
    auto it = std::find(scene_items_.begin(), scene_items_.end(), item);
    if (it != scene_items_.end()) {
        item->set_transform(transform);
        return ErrorCode::SUCCESS;
    }
    LOG_WARNING("Scene item does not belong to scene: " + name_);
    return ErrorCode::FAILURE;
}

// SceneManager实现
SceneManager::SceneManager() {
    LOG_INFO("Initialized SceneManager");
    // 创建默认场景
    create_scene("Default");
}

ErrorCode SceneManager::create_scene(const std::string& name) {
    // 检查场景是否已存在
    for (const auto& scene : scenes_) {
        if (scene->get_name() == name) {
            LOG_WARNING("Scene already exists: " + name);
            return ErrorCode::FAILURE;
        }
    }
    
    scenes_.push_back(std::make_shared<Scene>(name));
    LOG_INFO("Created scene: " + name);
    return ErrorCode::SUCCESS;
}

ErrorCode SceneManager::remove_scene(const std::string& name) {
    auto it = std::find_if(scenes_.begin(), scenes_.end(),
        [&name](const std::shared_ptr<Scene>& scene) {
            return scene->get_name() == name;
        });

    if (it != scenes_.end()) {
        // 在移除场景之前，确保所有SceneItem都被正确清理
        auto scene_items = (*it)->get_all_scene_items();
        for (auto& item : scene_items) {
            item->set_transform(Transform()); // 重置变换
            item->set_visible(false); // 隐藏
        }

        bool was_current_scene = (scenes_[current_scene_index_]->get_name() == name);
        scenes_.erase(it);
        LOG_INFO("Removed scene: " + name);

        // 如果我们移除了当前场景，选择第一个场景
        if (was_current_scene && !scenes_.empty()) {
            current_scene_index_ = 0;
        } else if (current_scene_index_ >= scenes_.size()) {
            current_scene_index_ = scenes_.empty() ? 0 : current_scene_index_;
        }

        return ErrorCode::SUCCESS;
    }

    LOG_WARNING("Scene not found: " + name);
    return ErrorCode::FAILURE;
}

ErrorCode SceneManager::set_current_scene(const std::string& name) {
    for (size_t i = 0; i < scenes_.size(); ++i) {
        if (scenes_[i]->get_name() == name) {
            current_scene_index_ = i;
            LOG_INFO("Set current scene to: " + name);
            return ErrorCode::SUCCESS;
        }
    }
    
    LOG_WARNING("Scene not found: " + name);
    return ErrorCode::FAILURE;
}

std::shared_ptr<Scene> SceneManager::get_current_scene() const {
    if (!scenes_.empty()) {
        return scenes_[current_scene_index_];
    }
    return nullptr;
}

std::vector<std::string> SceneManager::get_scene_names() const {
    std::vector<std::string> names;
    for (const auto& scene : scenes_) {
        names.push_back(scene->get_name());
    }
    return names;
}

std::shared_ptr<SceneItem> SceneManager::add_source_to_scene(const std::string& scene_name, std::shared_ptr<Source> source) {
    for (const auto& scene : scenes_) {
        if (scene->get_name() == scene_name) {
            return scene->add_source(source);
        }
    }
    
    LOG_WARNING("Scene not found: " + scene_name);
    return nullptr;
}

ErrorCode SceneManager::remove_source_from_scene(const std::string& scene_name, const std::string& source_id) {
    for (const auto& scene : scenes_) {
        if (scene->get_name() == scene_name) {
            return scene->remove_scene_item_by_source_id(source_id);
        }
    }
    
    LOG_WARNING("Scene not found: " + scene_name);
    return ErrorCode::FAILURE;
}

ErrorCode SceneManager::remove_scene_item_from_scene(const std::string& scene_name, std::shared_ptr<SceneItem> item) {
    for (const auto& scene : scenes_) {
        if (scene->get_name() == scene_name) {
            return scene->remove_scene_item(item);
        }
    }
    
    LOG_WARNING("Scene not found: " + scene_name);
    return ErrorCode::FAILURE;
}

std::vector<std::shared_ptr<SceneItem>> SceneManager::get_scene_items(const std::string& scene_name) const {
    for (const auto& scene : scenes_) {
        if (scene->get_name() == scene_name) {
            return scene->get_all_scene_items();
        }
    }
    LOG_WARNING("Scene not found: " + scene_name);
    return {};
}

// ------------ Layer helpers implementation --------------

ErrorCode Scene::move_scene_item_up(std::shared_ptr<SceneItem> item) {
    auto it = std::find(scene_items_.begin(), scene_items_.end(), item);
    if (it == scene_items_.end()) {
        LOG_WARNING("Scene item not found when moving up");
        return ErrorCode::FAILURE;
    }
    if (item->get_order() == 0) {
        return ErrorCode::SUCCESS; // Already at top (front-most)
    }
    const int target_order = item->get_order() - 1;
    for (auto& other : scene_items_) {
        if (other->get_order() == target_order) {
            other->set_order(other->get_order() + 1);
            break;
        }
    }
    item->set_order(target_order);
    normalize_orders();
    return ErrorCode::SUCCESS;
}

ErrorCode Scene::move_scene_item_down(std::shared_ptr<SceneItem> item) {
    auto it = std::find(scene_items_.begin(), scene_items_.end(), item);
    if (it == scene_items_.end()) {
        LOG_WARNING("Scene item not found when moving down");
        return ErrorCode::FAILURE;
    }
    const int max_order = static_cast<int>(scene_items_.size()) - 1;
    if (item->get_order() == max_order) {
        return ErrorCode::SUCCESS; // Already bottom (back-most)
    }
    const int target_order = item->get_order() + 1;
    for (auto& other : scene_items_) {
        if (other->get_order() == target_order) {
            other->set_order(other->get_order() - 1);
            break;
        }
    }
    item->set_order(target_order);
    normalize_orders();
    return ErrorCode::SUCCESS;
}

ErrorCode Scene::set_scene_item_order(std::shared_ptr<SceneItem> item, int new_order) {
    auto it = std::find(scene_items_.begin(), scene_items_.end(), item);
    if (it == scene_items_.end()) {
        LOG_WARNING("Scene item not found when setting order");
        return ErrorCode::FAILURE;
    }
    if (new_order < 0) new_order = 0;
    if (new_order >= static_cast<int>(scene_items_.size())) {
        new_order = static_cast<int>(scene_items_.size()) - 1;
    }
    for (auto& other : scene_items_) {
        if (other.get() == item.get()) continue;
        int order = other->get_order();
        if (order >= new_order) {
            other->set_order(order + 1);
        }
    }
    item->set_order(new_order);
    normalize_orders();
    return ErrorCode::SUCCESS;
}

void Scene::normalize_orders() {
    // sort by order then reassign contiguous
    std::sort(scene_items_.begin(), scene_items_.end(), [](const auto& a, const auto& b){ return a->get_order() < b->get_order();});
    int idx = 0;
    for (auto& it : scene_items_) {
        it->set_order(idx++);
    }
    next_order_ = idx;
}

} // namespace live_assistant
