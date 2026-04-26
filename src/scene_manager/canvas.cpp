#include "scene_manager/canvas.h"
#include "common/log.h"
#include "video_engine/video_engine.h"
#include "scene_manager/source_factory.h"
#include "scene_manager/render_utils.h"
#include "media_pipeline/media_file_source.h"
#include <QTimer>
#include <QBrush>
#include <QPen>
#include <QCursor>
#include <algorithm>

namespace live_assistant {

// CanvasRenderer实现
CanvasRenderer::CanvasRenderer() {
    LOG_INFO("CanvasRenderer initialized");
}

void CanvasRenderer::set_canvas_size(int w, int h) {
    if (w > 0 && h > 0) {
        canvas_width_ = w;
        canvas_height_ = h;
    }
}

void CanvasRenderer::render(QPainter& painter, const std::shared_ptr<Scene>& scene, const QRect& target_rect, const std::shared_ptr<SceneItem>& hovered_item) {
    if (!scene) {
        // 绘制空白背景
        painter.fillRect(target_rect, QBrush(QColor(15, 15, 15)));
        return;
    }

    // 绘制背景
    painter.fillRect(target_rect, QBrush(QColor(15, 15, 15)));

    // 获取所有场景项
    auto scene_items = scene->get_all_scene_items();

    // 按顺序(z-index)对场景项排序
    std::sort(scene_items.begin(), scene_items.end(),
        [](const std::shared_ptr<SceneItem>& a, const std::shared_ptr<SceneItem>& b) {
            return a->get_order() < b->get_order();
        });

    // 渲染每个场景项
    for (const auto& item : scene_items) {
        render_scene_item(painter, item, target_rect, hovered_item);
    }
}

void CanvasRenderer::render_scene_item(QPainter& painter, const std::shared_ptr<SceneItem>& item, const QRect& target_rect, const std::shared_ptr<SceneItem>& hovered_item) {
    if (!item || !item->is_visible()) {
        return;
    }

    // 获取变换和源
    auto transform = item->get_transform();
    auto source = item->get_source();

    // 将画布坐标转换为显示坐标
    auto mapping = compute_canvas_mapping(target_rect, canvas_width_, canvas_height_);

    // 转换坐标（将 transform 的 canvas 坐标映射到 target_rect）
    int x = static_cast<int>(transform.x * mapping.scale + mapping.offset_x);
    int y = static_cast<int>(transform.y * mapping.scale + mapping.offset_y);
    int width = static_cast<int>(transform.width * mapping.scale);
    int height = static_cast<int>(transform.height * mapping.scale);
    
    // 如果宽度或高度为0，设置默认值
    if (width == 0 || height == 0) {
        width = 640;
        height = 360;
    }
    
    // 绘制项
    QRect item_rect(x, y, width, height);
    
    // 设置绘制器不透明度
    painter.setOpacity(transform.opacity);
    
    // 对于视频捕获源，优先使用源自身提供的帧
    if (source->get_type() == Source::Type::VIDEO_CAPTURE) {
        bool frame_rendered = false;

        // 首先尝试 ScreenSource（屏幕共享）
        auto screenSrc = std::dynamic_pointer_cast<ScreenSource>(source);
        if (screenSrc) {
            QImage latest = screenSrc->get_latest_frame();
                if (!latest.isNull()) {
                    // Scale to fill, cropping if necessary, then draw the center part.
                // Use FastTransformation for better performance (SmoothTransformation is too slow for real-time)
                QImage scaled = latest.scaled(item_rect.size(), Qt::KeepAspectRatio, Qt::FastTransformation);
                QPoint top_left(
                    item_rect.x() + (item_rect.width() - scaled.width()) / 2,
                    item_rect.y() + (item_rect.height() - scaled.height()) / 2);
                painter.drawImage(top_left, scaled);
                frame_rendered = true;
            }
        }

        // 如果不是 ScreenSource，尝试 CameraSource
        if (!frame_rendered) {
            auto cameraSrc = std::dynamic_pointer_cast<CameraSource>(source);
            if (cameraSrc) {
                QImage latest = cameraSrc->get_latest_frame();
                if (!latest.isNull()) {
                    // 应用镜像设置（勾选镜像时水平翻转）
                    if (transform.mirror) {
                        latest = latest.mirrored(true, false);
                    }
                    // Scale to fill, cropping if necessary, then draw the center part.
                    QImage scaled = latest.scaled(item_rect.size(), Qt::KeepAspectRatio, Qt::FastTransformation);
                    QPoint top_left(
                        item_rect.x() + (item_rect.width() - scaled.width()) / 2,
                        item_rect.y() + (item_rect.height() - scaled.height()) / 2);
                    painter.drawImage(top_left, scaled);
                    frame_rendered = true;
                }
            }
        }

        // 如果没有来源帧，继续原有 VideoEngine 回退逻辑
        {
            CanvasWidget* widget = dynamic_cast<CanvasWidget*>(painter.device());
            if (widget) {
                auto video_engine = widget->get_video_engine();
                if (video_engine) {
                    // 获取最新的视频帧
                    auto frame = video_engine->get_latest_frame();
                    if (frame && frame->data) {
                        // 将VideoFrame转换为QImage
                        QImage image(frame->data.get(), frame->width, frame->height, frame->stride, QImage::Format_RGBA8888);
                        if (!image.isNull()) {
                            // 绘制视频帧，使用变换进行缩放和定位
                            // Scale to fill, cropping if necessary, then draw the center part.
                            QImage scaled = image.scaled(item_rect.size(), Qt::KeepAspectRatio, Qt::FastTransformation);
                            QPoint top_left(
                                item_rect.x() + (item_rect.width() - scaled.width()) / 2,
                                item_rect.y() + (item_rect.height() - scaled.height()) / 2);
                            painter.drawImage(top_left, scaled);
                        } else {
                            painter.fillRect(item_rect, QBrush(QColor(50, 150, 50)));
                        }
                    } else {
                        painter.fillRect(item_rect, QBrush(QColor(50, 150, 50)));
                    }
                } else {
                    painter.fillRect(item_rect, QBrush(QColor(50, 150, 50)));
                }
            } else {
                painter.fillRect(item_rect, QBrush(QColor(50, 150, 50)));
            }
        }
    } else if (source->get_type() == Source::Type::FILE_SOURCE) {
        // 处理文件源（插播视频）
        // 使用 get_latest_frame() 获取帧（无锁，安全）
        auto mediaSource = std::dynamic_pointer_cast<MediaFileSource>(source);
        bool is_running = mediaSource && mediaSource->is_running();

        // 诊断：统计获取帧的频率
        static int64_t last_get_time = 0;
        static int get_count = 0;
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        // 使用新的无锁方法获取帧
        QImage image = is_running ? mediaSource->get_latest_frame() : QImage();

        // 统计渲染频率
        get_count++;
        if (now - last_get_time >= 1000) {
            int null_count = 0;
            static int64_t last_null_time = 0;
            if (image.isNull()) {
                null_count++;
            }
            LOG_INFO("[CANVAS-FILE] render fps: " + std::to_string(get_count) +
                     ", is_running=" + std::string(is_running ? "true" : "false") +
                     ", image_null=" + std::string(image.isNull() ? "true" : "false"));
            get_count = 0;
            last_get_time = now;
        }

        // 只在首次成功获取帧时输出诊断日志
        static bool first_frame_logged = false;
        if (!image.isNull() && !first_frame_logged) {
            LOG_INFO("[CANVAS] FILE_SOURCE first frame: " + std::to_string(image.width()) + "x" + std::to_string(image.height()));
            first_frame_logged = true;
        }

        if (!image.isNull()) {
            // 安全检查：确保 item_rect 在有效范围内
            if (item_rect.width() > 0 && item_rect.height() > 0 &&
                item_rect.x() >= -item_rect.width() && item_rect.y() >= -item_rect.height()) {
                // 性能优化：避免每帧创建 scaled 临时大图
                QImage scaled = image.scaled(item_rect.size(), Qt::KeepAspectRatio, Qt::FastTransformation);
                QPoint top_left(
                    item_rect.x() + (item_rect.width() - scaled.width()) / 2,
                    item_rect.y() + (item_rect.height() - scaled.height()) / 2);
                painter.drawImage(top_left, scaled);
            }
        }
        // 如果没有帧，保持透明
    } else {
        QColor rect_color;
        switch (source->get_type()) {
            case Source::Type::AUDIO_CAPTURE:
                rect_color = QColor(200, 50, 50);
                break;
            case Source::Type::FILE_SOURCE:
                rect_color = QColor(200, 150, 50);
                break;
            case Source::Type::NETWORK_SOURCE:
                rect_color = QColor(150, 50, 200);
                break;
            default:
                rect_color = QColor(150, 150, 150);
                break;
        }
        
        // 绘制矩形
        painter.fillRect(item_rect, QBrush(rect_color));
    }
    
    // 移除源名称显示（右上角的序列号）
    // painter.setOpacity(1.0);
    // painter.setPen(QColor(255, 255, 255));
    // painter.drawText(x + 5, y + 15, QString::fromStdString(source->get_id()));

    return;
    
    // 标签绘制落点
RENDER_LABELS:;
    
    // 仅当hovered_item是当前项时，显示边界框和调整大小句柄
    if (item == hovered_item) {
        QRect mapped_rect = item_rect;

        // 如果启用，绘制边界框（使用映射后的坐标）
        if (show_bounding_boxes_) {
            QPen pen(QColor(0, 255, 255));
            pen.setWidth(2);
            pen.setStyle(Qt::DashLine);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(mapped_rect);
        }

        // 如果启用，绘制调整大小句柄（使用映射后的坐标）
        if (show_resize_handles_) {
            const int handle_size = 8;
            QBrush handle_brush(QColor(0, 255, 255));
            QPen handle_pen(QColor(0, 150, 255), 1);
            painter.setBrush(handle_brush);
            painter.setPen(handle_pen);

            std::vector<QPoint> handles = {
                QPoint(mapped_rect.left() - handle_size / 2, mapped_rect.top() - handle_size / 2),
                QPoint(mapped_rect.left() + mapped_rect.width() / 2 - handle_size / 2, mapped_rect.top() - handle_size / 2),
                QPoint(mapped_rect.right() - handle_size / 2, mapped_rect.top() - handle_size / 2),
                QPoint(mapped_rect.right() - handle_size / 2, mapped_rect.top() + mapped_rect.height() / 2 - handle_size / 2),
                QPoint(mapped_rect.right() - handle_size / 2, mapped_rect.bottom() - handle_size / 2),
                QPoint(mapped_rect.left() + mapped_rect.width() / 2 - handle_size / 2, mapped_rect.bottom() - handle_size / 2),
                QPoint(mapped_rect.left() - handle_size / 2, mapped_rect.bottom() - handle_size / 2),
                QPoint(mapped_rect.left() - handle_size / 2, mapped_rect.top() + mapped_rect.height() / 2 - handle_size / 2)
            };

            for (const auto& handle : handles) {
                painter.drawRect(handle.x(), handle.y(), handle_size, handle_size);
            }
        }
    }
    
    // 重置不透明度
    painter.setOpacity(1.0);
}

void CanvasRenderer::draw_bounding_box(QPainter& painter, const Transform& transform, bool selected) {
    // 计算矩形
    QRect rect(transform.x, transform.y, transform.width, transform.height);
    
    // 根据选择状态设置画笔颜色
    QPen pen(selected ? QColor(0, 255, 255) : QColor(255, 255, 255));
    pen.setWidth(2);
    pen.setStyle(Qt::DashLine);
    
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rect);
}

void CanvasRenderer::draw_resize_handles(QPainter& painter, const Transform& transform) {
    // 绘制8个调整大小句柄（角落和边缘）
    const int handle_size = 8;
    
    // 计算句柄位置
    std::vector<QPoint> handles = {
        // 左上
        QPoint(transform.x - handle_size / 2, transform.y - handle_size / 2),
        // 上中
        QPoint(transform.x + transform.width / 2 - handle_size / 2, transform.y - handle_size / 2),
        // 右上
        QPoint(transform.x + transform.width - handle_size / 2, transform.y - handle_size / 2),
        // 右中
        QPoint(transform.x + transform.width - handle_size / 2, transform.y + transform.height / 2 - handle_size / 2),
        // 右下
        QPoint(transform.x + transform.width - handle_size / 2, transform.y + transform.height - handle_size / 2),
        // 下中
        QPoint(transform.x + transform.width / 2 - handle_size / 2, transform.y + transform.height - handle_size / 2),
        // 左下
        QPoint(transform.x - handle_size / 2, transform.y + transform.height - handle_size / 2),
        // 左中
        QPoint(transform.x - handle_size / 2, transform.y + transform.height / 2 - handle_size / 2)
    };
    
    // 绘制句柄
    QBrush handle_brush(QColor(0, 255, 255));
    QPen handle_pen(QColor(0, 150, 255), 1);
    
    painter.setBrush(handle_brush);
    painter.setPen(handle_pen);
    
    for (const auto& handle : handles) {
        painter.drawRect(handle.x(), handle.y(), handle_size, handle_size);
    }
}

// CanvasWidget实现
CanvasWidget::CanvasWidget(QWidget *parent) : QWidget(parent) {
    // 初始化渲染器
    renderer_ = std::make_unique<CanvasRenderer>();

    // 设置组件属性
    setMinimumSize(800, 600);
    setMouseTracking(true);
    setCursor(Qt::ArrowCursor);

    // 创建刷新定时器 (30fps)
    // 使用 QTimer 定期触发刷新，避免 QTimer::singleShot 可能导致的事件队列积压
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this]() {
        // 记录定时器触发
        static int timer_count = 0;
        timer_count++;
        static int64_t last_timer_time = 0;
        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now_ms - last_timer_time >= 1000) {
            LOG_DEBUG("[DIAG] Timer tick, count=" + std::to_string(timer_count) + " interval=" + std::to_string(now_ms - last_timer_time));
            last_timer_time = now_ms;
            timer_count = 0;
        }

        // 直接调用 update()，让 Qt 自动合并多次刷新请求
        update();

        // 触发 Compositor 刷新
        if (compositor_) {
            compositor_->update();
        }
    });
    timer->start(33);

    LOG_INFO("CanvasWidget created with 30fps render timer");
}

CanvasWidget::~CanvasWidget() {
    LOG_INFO("CanvasWidget destroyed");
}

void CanvasWidget::reset_interaction_state() {
    is_dragging_ = false;
    is_resizing_ = false;
    resize_handle_ = 0;
    camera_resize_handle_ = -1;
    selected_item_.reset();
    hovered_item_.reset();
    last_mouse_pos_ = QPoint();
    original_transform_ = Transform();
    is_maximized_ = false;
    maximized_source_id_.clear();
    saved_item_rect_ = QRectF();
    saved_transform_ = Transform();
    camera_transform_ = Transform();
}

void CanvasWidget::set_scene_manager(std::shared_ptr<SceneManager> scene_manager) {
    reset_interaction_state();
    scene_manager_ = scene_manager;
    current_scene_.reset();
    if (!scene_manager_) {
        refresh();
        return;
    }
    
    // 如果有场景，将当前场景设置为第一个
    auto scene_names = scene_manager_->get_scene_names();
    if (!scene_names.empty()) {
        set_current_scene(scene_names[0]);
    } else {
        refresh();
    }
}

void CanvasWidget::set_current_scene(const std::string& scene_name) {
    reset_interaction_state();
    if (!scene_manager_) {
        current_scene_.reset();
        refresh();
        return;
    }
    
    // 查找场景
    auto scenes = scene_manager_->get_scene_names();
    bool scene_found = false;
    for (const auto& name : scenes) {
        if (name == scene_name) {
            scene_found = true;
            break;
        }
    }
    
    if (scene_found) {
        current_scene_ = scene_manager_->get_current_scene();
        LOG_INFO("Set current scene to: " + scene_name);
    } else {
        LOG_WARNING("Scene not found: " + scene_name);
    }
    
    // 刷新画布
    refresh();
}

void CanvasWidget::refresh() {
    update();
}

std::shared_ptr<Scene> CanvasWidget::get_current_scene() const {
    return current_scene_;
}

void CanvasWidget::set_interaction_enabled(bool enabled) {
    interaction_enabled_ = enabled;
}

bool CanvasWidget::is_interaction_enabled() const {
    return interaction_enabled_;
}

void CanvasWidget::set_video_engine(std::shared_ptr<VideoEngine> video_engine) {
    video_engine_ = video_engine;
    LOG_INFO("Set video engine for CanvasWidget");
}

void CanvasWidget::set_compositor(std::shared_ptr<Compositor> compositor) {
    compositor_ = compositor;
    if (compositor_) {
        compositor_->set_canvas_size(canvas_width_, canvas_height_);
        // 同时设置 renderer_ 的 compositor（用于从帧同步层获取插播视频帧）
        if (renderer_) {
            renderer_->set_compositor(compositor);
        }
        LOG_INFO("Set compositor for CanvasWidget");
    }
}

std::shared_ptr<Compositor> CanvasWidget::get_compositor() const {
    return compositor_;
}

void CanvasWidget::set_canvas_resolution(int width, int height) {
    canvas_width_ = width;
    canvas_height_ = height;
    LOG_INFO("Set canvas resolution: " + std::to_string(width) + "x" + std::to_string(height));

    // 同步更新compositor的画布大小，确保推流分辨率一致
    if (compositor_) {
        compositor_->set_canvas_size(width, height);
    }

    if (renderer_) {
        renderer_->set_canvas_size(width, height);
    }

    refresh();
}

void CanvasWidget::set_canvas_config(const CanvasConfig& config) {
    canvas_config_ = config;
    canvas_width_ = config.get_width();
    canvas_height_ = config.get_height();

    LOG_INFO("Set canvas config: " + config.get_name() + " (" +
             std::to_string(canvas_width_) + "x" + std::to_string(canvas_height_) + ")");

    // 同步更新compositor的画布大小，确保推流分辨率一致
    if (compositor_) {
        compositor_->set_canvas_size(canvas_width_, canvas_height_);
    }

    // 同步更新渲染器的逻辑画布尺寸，保证预览与推流映射一致
    if (renderer_) {
        renderer_->set_canvas_size(canvas_width_, canvas_height_);
    }

    // 更新组件的最小尺寸建议
    setMinimumSize(canvas_width_ / 4, canvas_height_ / 4); // 允许缩小到1/4

    refresh();
}

void CanvasWidget::paintEvent(QPaintEvent *event) {
    // 记录渲染时间戳
    static int64_t last_paint_time = 0;
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    static int paint_count = 0;
    paint_count++;

    // 诊断：测量渲染耗时
    auto render_start = std::chrono::high_resolution_clock::now();

    // 每秒打印一次诊断信息
    // 注意：每秒一次的文件 IO 也会对 UI 帧率产生轻微影响，生产环境建议关闭
    if (now_ms - last_paint_time >= 1000) {
        LOG_DEBUG("[DIAG] CanvasWidget::paintEvent called, count=" + std::to_string(paint_count));
        last_paint_time = now_ms;
    }

    QPainter painter(this);

    // 获取组件尺寸
    QRect widget_rect = this->rect();

    // 绘制背景
    painter.fillRect(widget_rect, QBrush(QColor(15, 15, 15)));


    // ═══════════════════════════════════════════════════════════════════
    // 方案A（已禁用）：使用 Compositor 进行统一渲染
    // 问题：Compositor 需要每个源主动更新帧数据，但摄像头和屏幕共享没有设置回调
    // 回退到原来的 renderer_ 逻辑，它能正确处理所有类型的源
    // ═══════════════════════════════════════════════════════════════════
    
    // 只渲染场景项（摄像头、屏幕共享、图片等）
    // 所有视频源都通过SceneItem在场景系统中统一渲染
    if (renderer_ && current_scene_) {
        // If a source is maximized, draw it with Fit (keep aspect ratio, show full image)
        if (is_maximized_ && !maximized_source_id_.empty()) {
            // Find the maximized scene item
            std::shared_ptr<SceneItem> maximized_item = nullptr;
            for (const auto& it : current_scene_->get_all_scene_items()) {
                if (it && it->get_source() && it->get_source()->get_id() == maximized_source_id_) {
                    maximized_item = it;
                    break;
                }
            }

            if (maximized_item) {
                // Try to draw the source's latest frame (handle ScreenSource and CameraSource)
                auto src = maximized_item->get_source();
                QImage latest;
                // ScreenSource and CameraSource expose get_latest_frame
                auto screenSrc = std::dynamic_pointer_cast<ScreenSource>(src);
                auto cameraSrc = std::dynamic_pointer_cast<CameraSource>(src);
                if (screenSrc) {
                    latest = screenSrc->get_latest_frame();
                } else if (cameraSrc) {
                    latest = cameraSrc->get_latest_frame();
                }

                if (!latest.isNull()) {
                    // Fit the image into the widget_rect (KeepAspectRatio)
                    QSize targetSize = widget_rect.size();
                    QImage scaled = latest.scaled(targetSize, Qt::KeepAspectRatio, Qt::FastTransformation);
                    int x = widget_rect.x() + (widget_rect.width() - scaled.width()) / 2;
                    int y = widget_rect.y() + (widget_rect.height() - scaled.height()) / 2;
                    QRect display_rect(x, y, scaled.width(), scaled.height());
                    painter.drawImage(display_rect, scaled);
                } else {
                    // Fallback to full scene render if we don't have latest frame
                    painter.save();
                    renderer_->render(painter, current_scene_, widget_rect, hovered_item_);
                    painter.restore();
                }
            } else {
                // If not found, render normally
                painter.save();
                renderer_->render(painter, current_scene_, widget_rect, hovered_item_);
                painter.restore();
            }
        } else {
            // 保存当前render状态
            painter.save();

            // 渲染整个场景，传递hovered_item
            renderer_->render(painter, current_scene_, widget_rect, hovered_item_);

            // 恢复render状态
            painter.restore();
        }
    }
    
    // 如果没有场景项，显示视频引擎的直接输出（保持向后兼容）
    if (video_engine_) {
        auto scene = current_scene_;
        if (!scene || scene->get_all_scene_items().empty()) {
            // 调用render_frame方法获取渲染后的帧
            auto frame = video_engine_->render_frame();
            if (frame && frame->data) {
                // 将VideoFrame转换为QImage
                QImage image(frame->data.get(), frame->width, frame->height, frame->stride, QImage::Format_RGBA8888);
                if (!image.isNull()) {
                    // 居中显示
                    int x = (widget_rect.width() - frame->width) / 2;
                    int y = (widget_rect.height() - frame->height) / 2;
                    QRect display_rect(x, y, frame->width, frame->height);
                    painter.drawImage(display_rect, image);
                }
            }
        }
    }

    // 诊断：记录渲染耗时
    auto render_end = std::chrono::high_resolution_clock::now();
    auto render_time = std::chrono::duration_cast<std::chrono::microseconds>(render_end - render_start).count();
    int64_t delta_ms = now_ms - last_paint_time;

    // 性能关键：不要每帧写日志（文件 IO 会阻塞 UI 线程，导致“渲染卡顿”假象）
    // 仅在慢帧或每秒采样一次记录。
    static int64_t last_diag_ms = 0;
    const bool slow_frame = (render_time > 8000) || (delta_ms > 100); // 8ms+ 或 UI 间隔异常
    if (slow_frame || (last_diag_ms == 0) || (now_ms - last_diag_ms >= 1000)) {
        LOG_DEBUG("[DIAG] Paint #" + std::to_string(paint_count) +
                 " render_us=" + std::to_string(render_time) +
                 " delta_ms=" + std::to_string(delta_ms));
        last_diag_ms = now_ms;
    }
}

std::shared_ptr<SceneItem> CanvasWidget::hit_test(int x, int y) const {
    if (!current_scene_) {
        return nullptr;
    }
    
    // 首先检查场景项（从上到下检查，z-index）
    auto scene_items = current_scene_->get_all_scene_items();
    
    // 反转列表，从上到下检查
    std::sort(scene_items.begin(), scene_items.end(),
        [](const std::shared_ptr<SceneItem>& a, const std::shared_ptr<SceneItem>& b) {
            if (!a || !b) {
                return static_cast<bool>(a);
            }
            return a->get_order() > b->get_order();
        });
    
    // compute mapping from canvas to widget coordinates
    QRect widget_rect = this->rect();
    auto mapping = compute_canvas_mapping(widget_rect, canvas_width_, canvas_height_);
    double scale = mapping.scale;
    double offset_x = mapping.offset_x;
    double offset_y = mapping.offset_y;

    for (const auto& item : scene_items) {
        auto transform = item->get_transform();
        QRect mapped_rect(
            static_cast<int>(transform.x * scale + offset_x),
            static_cast<int>(transform.y * scale + offset_y),
            static_cast<int>(transform.width * scale),
            static_cast<int>(transform.height * scale));

        if (mapped_rect.contains(x, y)) {
            return item;
        }

        // check handles using mapped_rect
        if (is_in_resize_handle(x, y, transform)) {
            return item;
        }
    }
    
    return nullptr;
}

bool CanvasWidget::is_in_resize_handle(int x, int y, const Transform& transform) const {
    int handle_index;
    return is_in_resize_handle(x, y, transform, handle_index);
}

bool CanvasWidget::is_in_resize_handle(int x, int y, const Transform& transform, int& handle_index) const {
    const int handle_size = 8;
    const int half_handle = handle_size / 2;
    
    // Map transform to widget coordinates then check all 8 resize handles
    QRect widget_rect = this->rect();
    auto mapping = compute_canvas_mapping(widget_rect, canvas_width_, canvas_height_);
    double scale = mapping.scale;
    double offset_x = mapping.offset_x;
    double offset_y = mapping.offset_y;

    int mx = static_cast<int>(transform.x * scale + offset_x);
    int my = static_cast<int>(transform.y * scale + offset_y);
    int mw = static_cast<int>(transform.width * scale);
    int mh = static_cast<int>(transform.height * scale);

    std::vector<QRect> handles = {
        QRect(mx - half_handle, my - half_handle, handle_size, handle_size),           // 0: 左上
        QRect(mx + mw / 2 - half_handle, my - half_handle, handle_size, handle_size),  // 1: 上中
        QRect(mx + mw - half_handle, my - half_handle, handle_size, handle_size),      // 2: 右上
        QRect(mx + mw - half_handle, my + mh / 2 - half_handle, handle_size, handle_size), // 3: 右中
        QRect(mx + mw - half_handle, my + mh - half_handle, handle_size, handle_size), // 4: 右下
        QRect(mx + mw / 2 - half_handle, my + mh - half_handle, handle_size, handle_size), // 5: 下中
        QRect(mx - half_handle, my + mh - half_handle, handle_size, handle_size),      // 6: 左下
        QRect(mx - half_handle, my + mh / 2 - half_handle, handle_size, handle_size)   // 7: 左中
    };
    
    // 检查鼠标是否在任何句柄中
    for (int i = 0; i < handles.size(); ++i) {
        if (handles[i].contains(x, y)) {
            handle_index = i;
            return true;
        }
    }
    
    handle_index = -1;
    return false;
}

bool CanvasWidget::is_in_camera_resize_handle(int x, int y, int& handle_index) const {
    const int handle_size = 8;
    const int half_handle = handle_size / 2;
    
    // Map camera_transform_ (canvas coords) to widget coords and check 4 corner handles
    QRect widget_rect = this->rect();
    auto mapping = compute_canvas_mapping(widget_rect, canvas_width_, canvas_height_);
    double scale = mapping.scale;
    double offset_x = mapping.offset_x;
    double offset_y = mapping.offset_y;

    int mx = static_cast<int>(camera_transform_.x * scale + offset_x);
    int my = static_cast<int>(camera_transform_.y * scale + offset_y);
    int mw = static_cast<int>(camera_transform_.width * scale);
    int mh = static_cast<int>(camera_transform_.height * scale);

    std::vector<QRect> handles = {
        QRect(mx - half_handle, my - half_handle, handle_size, handle_size),
        QRect(mx + mw - half_handle, my - half_handle, handle_size, handle_size),
        QRect(mx + mw - half_handle, my + mh - half_handle, handle_size, handle_size),
        QRect(mx - half_handle, my + mh - half_handle, handle_size, handle_size)
    };
    
    // 检查鼠标是否在任何句柄中
    for (int i = 0; i < handles.size(); ++i) {
        if (handles[i].contains(x, y)) {
            handle_index = i;
            return true;
        }
    }
    
    handle_index = -1;
    return false;
}

bool CanvasWidget::is_in_camera_resize_handle(int x, int y) const {
    int handle_index;
    return is_in_camera_resize_handle(x, y, handle_index);
}

void CanvasWidget::update_mouse_cursor(QMouseEvent *event) {
    if (!interaction_enabled_) {
        setCursor(Qt::ArrowCursor);
        return;
    }
    
    int x = event->pos().x();
    int y = event->pos().y();
    
    // 检查是否在场景项上
    auto item = hit_test(x, y);
    if (item) {
        // 检查是否在调整大小句柄上
        int handle_index;
        if (is_in_resize_handle(x, y, item->get_transform(), handle_index)) {
            // 根据句柄位置显示对应的双向箭头光标
            // 句柄索引: 0=左上, 1=上中, 2=右上, 3=右中, 4=右下, 5=下中, 6=左下, 7=左中
            switch (handle_index) {
                case 0: // 左上
                case 4: // 右下
                    setCursor(Qt::SizeFDiagCursor);  // 斜向双向箭头（/）
                    break;
                case 2: // 右上
                case 6: // 左下
                    setCursor(Qt::SizeBDiagCursor);  // 斜向双向箭头（\）
                    break;
                case 1: // 上中
                case 5: // 下中
                    setCursor(Qt::SizeVerCursor);    // 垂直双向箭头
                    break;
                case 3: // 右中
                case 7: // 左中
                    setCursor(Qt::SizeHorCursor);    // 水平双向箭头
                    break;
                default:
                    setCursor(Qt::SizeAllCursor);
                    break;
            }
            return;
        } else {
            // 鼠标在场景项内部，显示移动光标
            setCursor(Qt::SizeAllCursor);
            return;
        }
    }
    
    // 检查是否在摄像头视频帧上
    QRect camera_rect(camera_transform_.x, camera_transform_.y, camera_transform_.width, camera_transform_.height);
    if (camera_rect.contains(x, y)) {
        // 检查是否在摄像头调整大小句柄上
        int handle_index;
        if (is_in_camera_resize_handle(x, y, handle_index)) {
            // 根据不同角落显示不同的调整大小光标
            switch (handle_index) {
                case 0: // 左上
                case 2: // 右下
                    setCursor(Qt::SizeFDiagCursor);
                    break;
                case 1: // 右上
                case 3: // 左下
                    setCursor(Qt::SizeBDiagCursor);
                    break;
                default:
                    setCursor(Qt::SizeAllCursor);
                    break;
            }
            return;
        } else {
            // 鼠标在摄像头视频帧内部，显示十字星光标
            setCursor(Qt::CrossCursor);
            return;
        }
    }
    
    // 默认光标
    setCursor(Qt::ArrowCursor);
}

void CanvasWidget::mousePressEvent(QMouseEvent *event) {
    if (!interaction_enabled_) {
        return;
    }

    int x = event->pos().x();
    int y = event->pos().y();

    // 检查是否点击到场景项
    auto item = hit_test(x, y);

    if (item) {
        // 选择项
        selected_item_ = item;
        is_dragging_ = true;

        // 检查是否拖动调整大小句柄，并获取句柄索引
        int handle_index;
        is_resizing_ = is_in_resize_handle(x, y, item->get_transform(), handle_index);
        resize_handle_ = is_resizing_ ? handle_index + 1 : 0; // 1-8 表示句柄，0 表示不调整大小

        // 存储原始变换和鼠标位置
        original_transform_ = item->get_transform();
        last_mouse_pos_ = event->pos();

        // 发送选择信号
        emit scene_item_selected(item);

        // 更新画布
        refresh();
    } else {
        // 检查是否在摄像头视频帧上
        QRect camera_rect(camera_transform_.x, camera_transform_.y, camera_transform_.width, camera_transform_.height);
        if (camera_rect.contains(x, y)) {
            // 选择摄像头视频帧
            selected_item_ = nullptr; // 摄像头不是SceneItem，所以设为nullptr
            is_dragging_ = true;

            // 检查是否在调整大小句柄上
            int handle_index;
            is_resizing_ = is_in_camera_resize_handle(x, y, handle_index);
            camera_resize_handle_ = is_resizing_ ? handle_index : -1;

            // 存储原始变换和鼠标位置
            original_transform_ = camera_transform_;
            last_mouse_pos_ = event->pos();

            // 更新画布
            refresh();
        } else {
            // 点击空白处清除选择
            selected_item_ = nullptr;
            is_dragging_ = false;
            is_resizing_ = false;
            camera_resize_handle_ = -1;
            refresh();
        }
    }
}

void CanvasWidget::mouseMoveEvent(QMouseEvent *event) {
    // 更新鼠标悬停项
    std::shared_ptr<SceneItem> new_hovered_item = hit_test(event->pos().x(), event->pos().y());
    if (new_hovered_item != hovered_item_) {
        hovered_item_ = new_hovered_item;
        refresh();
    }
    
    if (!interaction_enabled_ || !is_dragging_) {
        update_mouse_cursor(event);
        return;
    }
    
    // 计算鼠标移动增量（widget 坐标）
    int delta_x = event->pos().x() - last_mouse_pos_.x();
    int delta_y = event->pos().y() - last_mouse_pos_.y();

    // 计算 widget->canvas 的缩放因子，并把 widget delta 转换为 canvas delta
    QRect widget_rect = this->rect();
    auto mapping = compute_canvas_mapping(widget_rect, canvas_width_, canvas_height_);
    int cdelta_x, cdelta_y;
    convert_widget_deltas_to_canvas(delta_x, delta_y, cdelta_x, cdelta_y, mapping);
    
    if (selected_item_) {
        // 处理SceneItem的拖动
        if (!current_scene_) {
            return;
        }
        
        // 获取当前变换
        auto transform = selected_item_->get_transform();
        Transform new_transform = transform;
        
        if (is_resizing_) {
            // 处理调整大小（使用 canvas deltas），根据句柄位置进行不同的缩放逻辑
            // resize_handle_: 1=左上, 2=上中, 3=右上, 4=右中, 5=右下, 6=下中, 7=左下, 8=左中
            switch (resize_handle_) {
                case 1: // 左上：调整x、y坐标和宽高
                    new_transform.x += cdelta_x;
                    new_transform.y += cdelta_y;
                    new_transform.width -= cdelta_x;
                    new_transform.height -= cdelta_y;
                    break;
                case 2: // 上中：调整y坐标和高度
                    new_transform.y += cdelta_y;
                    new_transform.height -= cdelta_y;
                    break;
                case 3: // 右上：调整y坐标、宽度、高度
                    new_transform.y += cdelta_y;
                    new_transform.width += cdelta_x;
                    new_transform.height -= cdelta_y;
                    break;
                case 4: // 右中：调整宽度
                    new_transform.width += cdelta_x;
                    break;
                case 5: // 右下：调整宽度和高度（默认行为）
                    new_transform.width += cdelta_x;
                    new_transform.height += cdelta_y;
                    break;
                case 6: // 下中：调整高度
                    new_transform.height += cdelta_y;
                    break;
                case 7: // 左下：调整x坐标、宽度、高度
                    new_transform.x += cdelta_x;
                    new_transform.width -= cdelta_x;
                    new_transform.height += cdelta_y;
                    break;
                case 8: // 左中：调整x坐标和宽度
                    new_transform.x += cdelta_x;
                    new_transform.width -= cdelta_x;
                    break;
                default:
                    new_transform.width += cdelta_x;
                    new_transform.height += cdelta_y;
                    break;
            }
            
            // 确保最小尺寸
            if (new_transform.width < 50) {
                new_transform.width = 50;
                // 如果宽度达到最小值，恢复x坐标
                if (resize_handle_ == 1 || resize_handle_ == 7 || resize_handle_ == 8) {
                    new_transform.x = transform.x + transform.width - 50;
                }
            }
            if (new_transform.height < 50) {
                new_transform.height = 50;
                // 如果高度达到最小值，恢复y坐标
                if (resize_handle_ == 1 || resize_handle_ == 2 || resize_handle_ == 3) {
                    new_transform.y = transform.y + transform.height - 50;
                }
            }
        } else {
            // 处理移动（使用 canvas deltas）
            new_transform.x += cdelta_x;
            new_transform.y += cdelta_y;
        }
        
        // 更新变换
        selected_item_->set_transform(new_transform);
        
        // 更新最后鼠标位置
        last_mouse_pos_ = event->pos();
        
        // 发送移动信号
        emit scene_item_moved(selected_item_, transform, new_transform);
        
        // 更新画布
        refresh();
    } else {
        // 处理摄像头视频帧的拖动或缩放
        Transform new_transform = camera_transform_;
        
            if (is_resizing_ && camera_resize_handle_ != -1) {
            // 处理缩放，根据不同的句柄索引实现不同的拉伸逻辑
            switch (camera_resize_handle_) {
                case 0: // 左上
                    // 调整x、y坐标和宽高
                    new_transform.x += cdelta_x;
                    new_transform.y += cdelta_y;
                    new_transform.width -= cdelta_x;
                    new_transform.height -= cdelta_y;
                    break;
                case 1: // 右上
                    // 调整y坐标、宽度，保持x坐标不变
                    new_transform.y += cdelta_y;
                    new_transform.width += cdelta_x;
                    new_transform.height -= cdelta_y;
                    break;
                case 2: // 右下
                    // 只调整宽高，保持x、y坐标不变
                    new_transform.width += cdelta_x;
                    new_transform.height += cdelta_y;
                    break;
                case 3: // 左下
                    // 调整x坐标、高度，保持y坐标不变
                    new_transform.x += cdelta_x;
                    new_transform.width -= cdelta_x;
                    new_transform.height += cdelta_y;
                    break;
                default:
                    // 未知句柄，使用默认缩放逻辑
                    new_transform.width += cdelta_x;
                    new_transform.height += cdelta_y;
                    break;
            }
            
            // 确保最小尺寸
            new_transform.width = (std::max)(100, new_transform.width);
            new_transform.height = (std::max)(100, new_transform.height);
            
            // 确保x、y坐标不会为负数
            new_transform.x = (std::max)(0, new_transform.x);
            new_transform.y = (std::max)(0, new_transform.y);
        } else {
            // 处理移动
            new_transform.x += delta_x;
            new_transform.y += delta_y;
        }
        
        // 更新摄像头变换
        camera_transform_ = new_transform;
        
        // 更新最后鼠标位置
        last_mouse_pos_ = event->pos();
        
        // 更新画布
        refresh();
    }
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent *event) {
    is_dragging_ = false;
    is_resizing_ = false;
    resize_handle_ = 0;
    camera_resize_handle_ = -1;
    update_mouse_cursor(event);
}

void CanvasWidget::mouseDoubleClickEvent(QMouseEvent *event) {
    if (!interaction_enabled_) return;

    // If already maximized, any double click restores.
    if (is_maximized_) {
        restore_item_from_maximize();
        return;
    }

    // Hit-test scene items first
    auto item = hit_test(event->pos().x(), event->pos().y());
    if (item) {
        maximize_item_in_canvas(item);
        return;
    }

    // Camera special-case: if double-click inside the camera rect, fullscreen the camera source if present.
    QRect camera_rect(camera_transform_.x, camera_transform_.y, camera_transform_.width, camera_transform_.height);
    if (camera_rect.contains(event->pos())) {
        // Try to locate a VIDEO_CAPTURE item (camera) in the scene and fullscreen it
        if (current_scene_) {
            for (auto &si : current_scene_->get_all_scene_items()) {
                auto src = si ? si->get_source() : nullptr;
                if (src && src->get_type() == Source::Type::VIDEO_CAPTURE) {
                    maximize_item_in_canvas(si);
                    return;
                }
            }
        }
    }
}

void CanvasWidget::keyPressEvent(QKeyEvent *event) {
    if (is_maximized_ && event->key() == Qt::Key_Escape) {
        restore_item_from_maximize();
        return;
    }
    QWidget::keyPressEvent(event);
}

void CanvasWidget::maximize_item_in_canvas(const std::shared_ptr<SceneItem>& item) {
    if (!item) return;
    auto src = item->get_source();
    if (!src) return;

    maximized_source_id_ = src->get_id();

    // Save complete transform (including mirror, rotation, opacity, etc.)
    auto old_t = item->get_transform();
    saved_transform_ = old_t;
    saved_item_rect_ = QRectF(old_t.x, old_t.y, old_t.width, old_t.height);

    // Maximize inside canvas (not OS fullscreen)
    Transform nt = old_t;
    nt.x = 0;
    nt.y = 0;
    nt.width = canvas_width_ > 0 ? canvas_width_ : width();
    nt.height = canvas_height_ > 0 ? canvas_height_ : height();
    // Preserve rotation, opacity, and mirror from original transform
    nt.rotation = old_t.rotation;
    nt.opacity = old_t.opacity;
    nt.mirror = old_t.mirror;
    item->set_transform(nt);

    // If this source is also a compositor layer (screen/window share), stretch that layer too.
    if (compositor_) {
        compositor_->set_layer_visible(maximized_source_id_, true);
        // Use canvas pixel resolution for compositor layer transform to keep
        // compositor output aligned with the canvas used for capture/encoding.
        int cw = canvas_width_ > 0 ? canvas_width_ : width();
        int ch = canvas_height_ > 0 ? canvas_height_ : height();
        compositor_->update_layer_transform(maximized_source_id_, QRectF(0, 0, cw, ch));
    }

    is_maximized_ = true;
    refresh();
}

void CanvasWidget::restore_item_from_maximize() {
    if (!is_maximized_) return;

    // Restore complete transform (including mirror, rotation, opacity, etc.)
    if (current_scene_) {
        for (auto &si : current_scene_->get_all_scene_items()) {
            auto src = si ? si->get_source() : nullptr;
            if (!src) continue;
            if (src->get_id() == maximized_source_id_) {
                si->set_transform(saved_transform_);
                break;
            }
        }
    }

    // Restore compositor layer transform best-effort (we don't have getters)
    if (compositor_ && !maximized_source_id_.empty()) {
        compositor_->update_layer_transform(maximized_source_id_, QRectF(saved_transform_.x, saved_transform_.y, saved_transform_.width, saved_transform_.height));
    }

    maximized_source_id_.clear();
    is_maximized_ = false;
    refresh();
}

} // namespace live_assistant
