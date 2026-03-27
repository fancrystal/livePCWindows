#include "scene_manager/scene_manager.h"
#include "scene_manager/source_factory.h"
#include "common/log.h"
#include "common/error.h"
#include <algorithm>

namespace live_assistant {

// Transform实现
Transform::Transform(int x, int y, int width, int height, float rotation, float opacity, bool mirror)
    : x(x), y(y), width(width), height(height), rotation(rotation), opacity(opacity), mirror(mirror) {
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

void SceneItem::set_device_id(const std::string& device_id) {
    device_id_ = device_id;
}

std::string SceneItem::get_device_id() const {
    return device_id_;
}

void SceneItem::set_display_name(const std::string& display_name) {
    display_name_ = display_name;
}

std::string SceneItem::get_display_name() const {
    return display_name_;
}

void SceneItem::set_source_params(const std::unordered_map<std::string, std::string>& params) {
    source_params_ = params;
}

std::unordered_map<std::string, std::string> SceneItem::get_source_params() const {
    return source_params_;
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

ErrorCode Scene::set_name(const std::string& name) {
    if (name.empty()) {
        LOG_WARNING("Scene name cannot be empty");
        return ErrorCode::FAILURE;
    }
    name_ = name;
    LOG_INFO("Scene renamed to: " + name);
    return ErrorCode::SUCCESS;
}

QJsonObject Scene::serialize() const {
    QJsonObject scene_obj;
    scene_obj["name"] = QString::fromStdString(name_);

    QJsonArray items_array;
    for (const auto& item : scene_items_) {
        QJsonObject item_obj;
        item_obj["source_id"] = QString::fromStdString(item->get_source_id());
        item_obj["visible"] = item->is_visible();
        item_obj["order"] = item->get_order();

        // 保存源类型和设备参数
        auto source = item->get_source();
        if (source) {
            // 保存设备ID和显示名称（用于重建采集源）
            item_obj["device_id"] = QString::fromStdString(item->get_device_id());
            item_obj["display_name"] = QString::fromStdString(item->get_display_name());

            // 保存源参数（分辨率、帧率等）
            const auto& params = item->get_source_params();
            if (!params.empty()) {
                QJsonObject params_obj;
                for (const auto& [key, value] : params) {
                    params_obj[QString::fromStdString(key)] = QString::fromStdString(value);
                }
                item_obj["source_params"] = params_obj;
            }
        }

        const auto& tr = item->get_transform();
        QJsonObject transform_obj;
        transform_obj["x"] = tr.x;
        transform_obj["y"] = tr.y;
        transform_obj["width"] = tr.width;
        transform_obj["height"] = tr.height;
        transform_obj["rotation"] = tr.rotation;
        transform_obj["opacity"] = tr.opacity;
        transform_obj["mirror"] = tr.mirror;
        item_obj["transform"] = transform_obj;

        items_array.append(item_obj);
    }
    scene_obj["items"] = items_array;

    return scene_obj;
}

std::shared_ptr<SceneItem> Scene::deserialize_item(const QJsonObject& obj) {
    // 这个方法需要外部传入Source，暂时返回nullptr的占位
    // 实际使用时会在SceneManager::deserialize中调用SourceFactory创建Source
    Q_UNUSED(obj);
    return nullptr;
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
    // 特殊处理：如果是摄像头源，设置为最高order值
    if (source->get_id().find("camera_") == 0) {
        // 为摄像头设置最高的order值
        int max_order = next_order_;
        for (const auto& existing_item : scene_items_) {
            if (existing_item->get_order() > max_order) {
                max_order = existing_item->get_order();
            }
        }
        item->set_order(max_order + 1);
        next_order_ = max_order + 2;
    } else {
        item->set_order(next_order_++);
    }
    
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
    // 创建默认场景（场景一）
    create_scene("场景一");
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
            // 修复：越界时修正为最后一个有效索引，原逻辑保留越界值会导致崩溃
            current_scene_index_ = scenes_.empty() ? 0 : static_cast<int>(scenes_.size()) - 1;
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

std::shared_ptr<SceneItem> SceneManager::add_source_to_scene(const std::string& scene_name, std::shared_ptr<Source> source,
                                                             const std::string& device_id,
                                                             const std::unordered_map<std::string, std::string>& params) {
    for (const auto& scene : scenes_) {
        if (scene->get_name() == scene_name) {
            auto item = scene->add_source(source);
            if (item) {
                item->set_device_id(device_id);
                item->set_source_params(params);
            }
            return item;
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

QJsonArray SceneManager::serialize() const {
    QJsonArray scenes_array;
    for (size_t i = 0; i < scenes_.size(); ++i) {
        QJsonObject scene_obj = scenes_[i]->serialize();
        scene_obj["current"] = (i == current_scene_index_);
        scenes_array.append(scene_obj);
    }
    return scenes_array;
}

std::shared_ptr<Source> SceneManager::create_source_by_type(const std::string& type, const std::string& id,
                                                             const std::string& device_id, const std::string& file_path) {
    if (type == "camera") {
        return SourceFactory::create_camera_source(id, device_id);
    } else if (type == "screen") {
        return SourceFactory::create_screen_source(id, device_id);
    } else if (type == "image") {
        return SourceFactory::create_image_source(id, file_path, "");
    } else if (type == "media_file") {
        return SourceFactory::create_media_file_source(id, file_path, "");
    } else if (type == "audio") {
        return SourceFactory::create_audio_source(id, "");
    }
    LOG_WARNING("Unknown source type: " + type);
    return nullptr;
}

ErrorCode SceneManager::deserialize(const QJsonArray& scenes_array) {
    // 清空现有场景
    scenes_.clear();

    std::string current_scene_name;
    bool has_current = false;

    for (const auto& scene_val : scenes_array) {
        QJsonObject scene_obj = scene_val.toObject();
        QString name = scene_obj["name"].toString();
        bool is_current = scene_obj["current"].toBool(false);

        // 创建场景
        create_scene(name.toStdString());

        if (is_current) {
            current_scene_name = name.toStdString();
            has_current = true;
        }

        // 加载场景项
        QJsonArray items_array = scene_obj["items"].toArray();
        for (const auto& item_val : items_array) {
            QJsonObject item_obj = item_val.toObject();
            QString source_id = item_obj["source_id"].toString();
            bool visible = item_obj["visible"].toBool(true);
            int order = item_obj["order"].toInt(0);

            // 读取保存的设备ID和参数（兼容旧配置格式）
            QString saved_device_id = "";
            QString saved_display_name = "";
            std::unordered_map<std::string, std::string> saved_params;
            if (item_obj.contains("device_id")) {
                saved_device_id = item_obj["device_id"].toString();
            }
            if (item_obj.contains("display_name")) {
                saved_display_name = item_obj["display_name"].toString();
            }
            if (item_obj.contains("source_params")) {
                QJsonObject params_obj = item_obj["source_params"].toObject();
                for (auto it = params_obj.begin(); it != params_obj.end(); ++it) {
                    saved_params[it.key().toStdString()] = it.value().toString().toStdString();
                }
            }

            // 从source_id解析类型
            QString source_type;
            QString device_or_file;

            if (source_id.startsWith("camera_")) {
                source_type = "camera";
                // 优先使用保存的device_id，否则从source_id解析
                if (!saved_device_id.isEmpty()) {
                    device_or_file = saved_device_id;
                } else {
                    device_or_file = source_id.mid(7); // 去掉 "camera_" 前缀
                }
            } else if (source_id.startsWith("capture_")) {
                // capture_G2412WHI 格式是屏幕共享，映射到 screen 类型
                source_type = "screen";
                if (!saved_device_id.isEmpty()) {
                    device_or_file = saved_device_id;
                } else {
                    device_or_file = source_id.mid(8); // 去掉 "capture_" 前缀
                }
            } else if (source_id.startsWith("screen_")) {
                source_type = "screen";
                if (!saved_device_id.isEmpty()) {
                    device_or_file = saved_device_id;
                } else {
                    device_or_file = source_id.mid(7);
                }
            } else if (source_id.startsWith("image_")) {
                source_type = "image";
                device_or_file = source_id.mid(6);
            } else if (source_id.startsWith("media_file_")) {
                source_type = "media_file";
                device_or_file = source_id.mid(11);
            } else if (source_id.startsWith("audio_")) {
                source_type = "audio";
                device_or_file = source_id.mid(6);
            } else {
                // 未知类型，跳过
                LOG_WARNING("Unknown source type in deserialization: " + source_id.toStdString());
                continue;
            }

            LOG_INFO("Deserializing source: type=" + source_type.toStdString() + ", device=" + device_or_file.toStdString());

            // 创建Source
            auto source = create_source_by_type(source_type.toStdString(),
                                                source_id.toStdString(),
                                                device_or_file.toStdString(),
                                                device_or_file.toStdString());
            if (!source) {
                LOG_WARNING("Failed to create source: " + source_id.toStdString());
                continue;
            }

            // 尝试初始化并启动采集源
            bool source_started = false;
            try {
                if (source->initialize()) {
                    if (source->start()) {
                        source_started = true;
                    } else {
                        LOG_WARNING("Failed to start source: " + source_id.toStdString());
                    }
                } else {
                    LOG_WARNING("Failed to initialize source: " + source_id.toStdString());
                }
            } catch (const std::exception& e) {
                LOG_WARNING("Exception starting source " + source_id.toStdString() + ": " + e.what());
            } catch (...) {
                LOG_WARNING("Unknown exception starting source: " + source_id.toStdString());
            }

            // 如果源启动失败，删除该source item并不添加到场景
            if (!source_started) {
                LOG_WARNING("Source not available, removing from scene: " + source_id.toStdString());
                // 尝试关闭并销毁source
                try {
                    source->stop();
                    source->shutdown();
                } catch (...) {}
                continue;
            }

            // 添加到场景（同时保存设备ID和参数）
            auto scene_item = add_source_to_scene(name.toStdString(), source,
                                                   device_or_file.toStdString(), saved_params);
            if (scene_item) {
                // 设置显示名称
                if (!saved_display_name.isEmpty()) {
                    scene_item->set_display_name(saved_display_name.toStdString());
                }
                // 设置可见性
                scene_item->set_visible(visible);
                scene_item->set_order(order);

                // 设置变换
                QJsonObject tr_obj = item_obj["transform"].toObject();
                Transform tr;
                tr.x = tr_obj["x"].toInt(0);
                tr.y = tr_obj["y"].toInt(0);
                tr.width = tr_obj["width"].toInt(640);
                tr.height = tr_obj["height"].toInt(360);
                tr.rotation = static_cast<float>(tr_obj["rotation"].toDouble(0.0));
                tr.opacity = static_cast<float>(tr_obj["opacity"].toDouble(1.0));
                tr.mirror = tr_obj["mirror"].toBool(false);
                scene_item->set_transform(tr);
            }
        }
    }

    // 设置当前场景
    if (has_current && !current_scene_name.empty()) {
        set_current_scene(current_scene_name);
    } else if (!scenes_.empty()) {
        set_current_scene(scenes_[0]->get_name());
    }

    LOG_INFO("Loaded " + std::to_string(scenes_.size()) + " scenes from config");
    return ErrorCode::SUCCESS;
}

void SceneManager::cleanup_all_sources() {
    LOG_INFO("========== SceneManager cleanup_all_sources START ==========");

    for (const auto& scene : scenes_) {
        if (!scene) continue;

        auto items = scene->get_all_scene_items();
        for (const auto& item : items) {
            if (!item) continue;

            auto source = item->get_source();
            if (!source) continue;

            LOG_INFO("Stopping source in scene: " + scene->get_name() + ", source: " + source->get_id());
            source->stop();
            source->shutdown();
        }
    }

    LOG_INFO("========== SceneManager cleanup_all_sources END ==========");
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
    // 分离摄像头项和其他项
    std::vector<std::shared_ptr<SceneItem>> camera_items;
    std::vector<std::shared_ptr<SceneItem>> other_items;
    
    for (const auto& item : scene_items_) {
        if (item->get_source_id().find("camera_") == 0) {
            camera_items.push_back(item);
        } else {
            other_items.push_back(item);
        }
    }
    
    // 对其他项按order排序并重新分配连续的order值
    std::sort(other_items.begin(), other_items.end(), [](const auto& a, const auto& b){ return a->get_order() < b->get_order();});
    int idx = 0;
    for (auto& it : other_items) {
        it->set_order(idx++);
    }
    
    // 对摄像头项按order排序并分配比其他项更高的order值
    std::sort(camera_items.begin(), camera_items.end(), [](const auto& a, const auto& b){ return a->get_order() < b->get_order();});
    for (auto& it : camera_items) {
        it->set_order(idx++);
    }
    
    next_order_ = idx;
}

} // namespace live_assistant
