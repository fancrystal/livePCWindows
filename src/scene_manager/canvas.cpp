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
        painter.fillRect(target_rect, QBrush(QColor(40, 40, 40)));
        return;
    }

    // 绘制背景
    painter.fillRect(target_rect, QBrush(QColor(40, 40, 40)));

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
                QImage scaled = latest.scaled(item_rect.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                QRectF source_rect((scaled.width() - item_rect.width()) / 2.0,
                                   (scaled.height() - item_rect.height()) / 2.0,
                                   item_rect.width(),
                                   item_rect.height());
                painter.drawImage(item_rect, scaled, source_rect);
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
                    QImage scaled = latest.scaled(item_rect.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                    QRectF source_rect((scaled.width() - item_rect.width()) / 2.0,
                                       (scaled.height() - item_rect.height()) / 2.0,
                                       item_rect.width(),
                                       item_rect.height());
                    painter.drawImage(item_rect, scaled, source_rect);
                    frame_rendered = true;
                }
            }
        }

        if (frame_rendered) {
            // 跳到绘制标签（后续统一处理）
            goto RENDER_LABELS;
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
                            QImage scaled = image.scaled(item_rect.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                            QRectF source_rect((scaled.width() - item_rect.width()) / 2.0,
                                               (scaled.height() - item_rect.height()) / 2.0,
                                               item_rect.width(),
                                               item_rect.height());
                            painter.drawImage(item_rect, scaled, source_rect);
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
        auto mediaSource = std::dynamic_pointer_cast<MediaFileSource>(source);
        bool is_running = mediaSource && mediaSource->is_running();
        auto frame = is_running ? mediaSource->get_video_frame() : nullptr;
        bool has_frame = frame && frame->data;

        if (is_running && has_frame) {
            // 将VideoFrame转换为QImage并绘制
            // 性能优化：使用 FastTransformation 代替 SmoothTransformation
            QImage image(frame->data.get(), frame->width, frame->height, frame->stride, QImage::Format_RGBA8888);
            if (!image.isNull()) {
                QImage scaled = image.scaled(item_rect.size(), Qt::KeepAspectRatioByExpanding, Qt::FastTransformation);
                QRectF source_rect((scaled.width() - item_rect.width()) / 2.0,
                                   (scaled.height() - item_rect.height()) / 2.0,
                                   item_rect.width(),
                                   item_rect.height());
                painter.drawImage(item_rect, scaled, source_rect);
                goto RENDER_LABELS;
            }
        }

        // 如果没有帧，保持透明（不绘制，让背景透出来）
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
    
        // 绘制源名称
    painter.setOpacity(1.0);
    painter.setPen(QColor(255, 255, 255));
    painter.drawText(x + 5, y + 15, QString::fromStdString(source->get_id()));

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
    // 使用 QTimer::singleShot 循环调用，确保每次都独立触发，避免 Qt 节流
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this, timer]() {
        // 记录定时器触发
        static int timer_count = 0;
        timer_count++;
        static int64_t last_timer_time = 0;
        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now_ms - last_timer_time >= 1000) {
            LOG_INFO("[DIAG] Timer tick, count=" + std::to_string(timer_count) +
                     ", isVisible=" + std::to_string(isVisible()) +
                     ", updatesEnabled=" + std::to_string(updatesEnabled()));
            last_timer_time = now_ms;
        }
        // 使用 QTimer::singleShot 触发刷新，确保 update() 被处理
        QTimer::singleShot(0, this, [this]() {
            update();
        });
        // 同时触发 Compositor 刷新（如果存在）
        if (compositor_) {
            QTimer::singleShot(0, compositor_.get(), [this]() {
                if (compositor_) {
                    compositor_->update();
                }
            });
        }
        // 重新启动定时器（保持恒定帧率）
        timer->start(33);
    });
    timer->start(33);

    LOG_INFO("CanvasWidget created with 30fps render timer (using QTimer::singleShot)");
}

CanvasWidget::~CanvasWidget() {
    LOG_INFO("CanvasWidget destroyed");
}

void CanvasWidget::set_scene_manager(std::shared_ptr<SceneManager> scene_manager) {
    scene_manager_ = scene_manager;
    
    // 如果有场景，将当前场景设置为第一个
    if (scene_manager_) {
        auto scene_names = scene_manager_->get_scene_names();
        if (!scene_names.empty()) {
            set_current_scene(scene_names[0]);
        }
    }
}

void CanvasWidget::set_current_scene(const std::string& scene_name) {
    if (!scene_manager_) {
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

    // 每秒打印一次诊断信息
    if (now_ms - last_paint_time >= 1000) {
        LOG_INFO("[DIAG] CanvasWidget::paintEvent called, count=" + std::to_string(paint_count) +
                 ", delta_ms=" + std::to_string(now_ms - last_paint_time));
        last_paint_time = now_ms;
    }

    QPainter painter(this);

    // 获取组件尺寸
    QRect widget_rect = this->rect();

    // 绘制背景
    painter.fillRect(widget_rect, QBrush(QColor(40, 40, 40)));


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
                    QImage scaled = latest.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
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
                QImage image(frame->data.get(   ), frame->width, frame->height, frame->stride, QImage::Format_RGBA8888);
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
}

std::shared_ptr<SceneItem> CanvasWidget::hit_test(int x, int y) const {
    if (!current_scene_) {
        return nullptr;
    }
    
    // 首先检查场景项（从上到下检查，z-index）
    auto scene_items = current_scene_->get_all_scene_items();
    
    // 反转列表，从上到下检查
    std::reverse(scene_items.begin(), scene_items.end());
    
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
        QRect(mx - half_handle, my - half_handle, handle_size, handle_size),
        QRect(mx + mw / 2 - half_handle, my - half_handle, handle_size, handle_size),
        QRect(mx + mw - half_handle, my - half_handle, handle_size, handle_size),
        QRect(mx + mw - half_handle, my + mh / 2 - half_handle, handle_size, handle_size),
        QRect(mx + mw - half_handle, my + mh - half_handle, handle_size, handle_size),
        QRect(mx + mw / 2 - half_handle, my + mh - half_handle, handle_size, handle_size),
        QRect(mx - half_handle, my + mh - half_handle, handle_size, handle_size),
        QRect(mx - half_handle, my + mh / 2 - half_handle, handle_size, handle_size)
    };
    
    // 检查鼠标是否在任何句柄中
    for (const auto& handle : handles) {
        if (handle.contains(x, y)) {
            return true;
        }
    }
    
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
        if (is_in_resize_handle(x, y, item->get_transform())) {
            setCursor(Qt::SizeAllCursor);
            return;
        } else {
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

        // 检查是否拖动调整大小句柄
        is_resizing_ = is_in_resize_handle(x, y, item->get_transform());

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
            // 处理调整大小（使用 canvas deltas）
            new_transform.width += cdelta_x;
            new_transform.height += cdelta_y;
            
            // 确保最小尺寸
            new_transform.width = (std::max)(50, new_transform.width);
            new_transform.height = (std::max)(50, new_transform.height);
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
