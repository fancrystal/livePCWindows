#pragma once

#include <QWidget>
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <memory>
#include <vector>
#include <unordered_map>

#include "scene_manager/scene_manager.h"
#include "scene_manager/compositor.h"
#include "common/config_manager.h"

namespace live_assistant {

// 前向声明
class SceneManager;
class CanvasRenderer;
class VideoEngine;

// Canvas组件，用于渲染场景和处理用户交互
// 该组件负责：
// 1. 渲染当前场景
// 2. 处理鼠标事件，用于源操作
// 3. 提供实时预览
class CanvasWidget : public QWidget {
    Q_OBJECT

public:
    explicit CanvasWidget(QWidget *parent = nullptr);
    ~CanvasWidget() override;
    
    // 设置该画布使用的场景管理器
    void set_scene_manager(std::shared_ptr<SceneManager> scene_manager);
    
    // 设置要渲染的当前场景
    void set_current_scene(const std::string& scene_name);
    
    // 手动刷新画布
    void refresh();
    
    // 获取当前场景
    std::shared_ptr<Scene> get_current_scene() const;
    
    // 启用/禁用交互
    void set_interaction_enabled(bool enabled);
    bool is_interaction_enabled() const;
    
    // 设置画布分辨率
    void set_canvas_resolution(int width, int height);

    // 设置画布配置（支持不同比例）
    void set_canvas_config(const CanvasConfig& config);
    const CanvasConfig& get_canvas_config() const { return canvas_config_; }
    
    // 设置视频引擎，用于直接显示摄像头帧
    void set_video_engine(std::shared_ptr<VideoEngine> video_engine);

    // 获取视频引擎
    std::shared_ptr<VideoEngine> get_video_engine() const { return video_engine_; }

    // Compositor 相关方法
    void set_compositor(std::shared_ptr<Compositor> compositor);
    std::shared_ptr<Compositor> get_compositor() const;
    
    // 获取 CanvasRenderer（用于推流捕获）
    CanvasRenderer* get_renderer() const { return renderer_.get(); }
    
signals:
    // 场景项被选中时发出的信号
    void scene_item_selected(std::shared_ptr<SceneItem> item);
    
    // 场景项被移动时发出的信号
    void scene_item_moved(std::shared_ptr<SceneItem> item, const Transform& old_transform, const Transform& new_transform);

    // FILE_SOURCE 场景项被单击时发出（用于暂停/恢复插播视频）
    // 双击行为已恢复为全屏/最大化
    void insert_video_single_clicked(std::shared_ptr<SceneItem> item);
    
protected:
    // 重写Qt绘制事件
    void paintEvent(QPaintEvent *event) override;
    
    // 鼠标事件处理程序
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    
private:
    void reset_interaction_state();
    void maximize_item_in_canvas(const std::shared_ptr<SceneItem>& item);
    void restore_item_from_maximize();
    bool is_maximized_ = false;
    std::string maximized_source_id_;
    QRectF saved_item_rect_;
    Transform saved_transform_; // 保存完整的Transform（包括镜像、旋转、不透明度等）


    // 辅助方法
    std::shared_ptr<SceneItem> hit_test(int x, int y) const;
    bool is_in_resize_handle(int x, int y, const Transform& transform) const;
    bool is_in_resize_handle(int x, int y, const Transform& transform, int& handle_index) const;
    bool is_in_camera_resize_handle(int x, int y, int& handle_index) const;
    bool is_in_camera_resize_handle(int x, int y) const;
    void update_mouse_cursor(QMouseEvent *event);  
    // 场景和渲染
    std::shared_ptr<SceneManager> scene_manager_;
    std::shared_ptr<Scene> current_scene_;
    std::unique_ptr<CanvasRenderer> renderer_;
    std::shared_ptr<VideoEngine> video_engine_; // 视频引擎，用于显示摄像头帧
    std::shared_ptr<Compositor> compositor_;    // 合成器，用于多源渲染

    // 定时器：用于自动刷新画布（插播视频等被动数据源需要）
    QTimer* refresh_timer_ = nullptr;

    // 画布属性
    CanvasConfig canvas_config_ = CanvasConfig::get_default();
    int canvas_width_ = 1280;
    int canvas_height_ = 720;
    bool interaction_enabled_ = true;
    
    // 鼠标交互状态
    bool is_dragging_ = false;
    bool is_resizing_ = false;
    std::shared_ptr<SceneItem> selected_item_;
    std::shared_ptr<SceneItem> hovered_item_; // 当前悬停的场景项
    QPoint last_mouse_pos_;
    QPoint press_pos_;           // 鼠标按下位置，用于区分单击和拖动
    Transform original_transform_;
    // 单击/双击消歧 timer（基于系统双击间隔）
    // Qt 双击序列：Press → Release(1) → DoubleClick → Release(2)
    // - Release(1)：启动 timer
    // - mouseDoubleClickEvent：停掉 timer + 设置 suppress_release_timer_ 标志
    // - Release(2)：发现标志位，跳过 timer 启动并清除标志
    // 这样 timer 只会在真正的单击后到期并发出信号。
    QTimer* click_debounce_timer_ = nullptr;
    std::shared_ptr<SceneItem> pending_click_item_;
    bool suppress_release_timer_ = false;  // 抑制 Release(2) 重启 timer
    
    // 调整大小句柄状态
    int resize_handle_ = 0; // 0 = 不调整大小, 1-8 = 调整大小句柄
    
    // 摄像头调整大小句柄索引
    int camera_resize_handle_ = -1; // -1 = 不调整大小, 0-3 = 四个角落的调整大小句柄
    
    // 摄像头视频帧的变换信息（用于拖动和调整大小）
    Transform camera_transform_;
};

// 用于渲染场景的Canvas渲染器
// 该类负责实际的渲染逻辑
class CanvasRenderer {
public:
    CanvasRenderer();
    ~CanvasRenderer() = default;
    
    // 将场景渲染到QPainter
    void render(QPainter& painter, const std::shared_ptr<Scene>& scene, const QRect& target_rect, const std::shared_ptr<SceneItem>& hovered_item = nullptr);
    
    // 渲染单个场景项
    void render_scene_item(QPainter& painter, const std::shared_ptr<SceneItem>& item, const QRect& target_rect, const std::shared_ptr<SceneItem>& hovered_item = nullptr);
    
    // 设置画布逻辑大小（用于将场景坐标映射到显示/编码分辨率）
    void set_canvas_size(int width, int height);

    // 设置渲染属性
    void set_show_bounding_boxes(bool show) { show_bounding_boxes_ = show; }
    void set_show_resize_handles(bool show) { show_resize_handles_ = show; }
    
    // 获取渲染属性
    bool is_showing_bounding_boxes() const { return show_bounding_boxes_; }
    bool is_showing_resize_handles() const { return show_resize_handles_; }
    
    // 设置 Compositor（用于从帧同步层获取插播视频帧）
    void set_compositor(std::shared_ptr<Compositor> compositor) { compositor_ = compositor; }
    
private:
    // Compositor（用于从帧同步层获取插播视频帧）
    std::shared_ptr<Compositor> compositor_;
    
    // 辅助方法
    void draw_bounding_box(QPainter& painter, const Transform& transform, bool selected);
    void draw_resize_handles(QPainter& painter, const Transform& transform);
    
    // 渲染设置
    bool show_bounding_boxes_ = true;
    bool show_resize_handles_ = true;
    
    // 逻辑画布尺寸（用于将场景坐标映射到显示/编码分辨率）
    int canvas_width_ = 1920;
    int canvas_height_ = 1080;
};

} // namespace live_assistant
