#include "scene_manager/compositor.h"
#include "common/log.h"

#include <QImage>

namespace live_assistant {

Compositor::Compositor(QWidget* parent)
    : QWidget(parent), canvas_size_(1920, 1080) {
    setMinimumSize(320, 240);
    LOG_INFO("Compositor created");
}

Compositor::~Compositor() {
    LOG_INFO("Compositor destroyed");
}

void Compositor::add_layer(const std::string& source_id) {
    std::lock_guard<std::mutex> lock(layers_mutex_);

    if (layers_.find(source_id) != layers_.end()) {
        LOG_WARNING("Layer already exists: " + source_id);
        return;
    }

    CompositorLayer layer;
    layer.source_id = source_id;
    layer.visible = true;
    layer.opacity = 1.0f;
    layer.dest_rect = QRectF(0, 0, canvas_size_.width(), canvas_size_.height());

    // Assign a z-order that places it on top
    int max_z = -1;
    for (const auto& pair : layers_) {
        if (pair.second.z_order > max_z) {
            max_z = pair.second.z_order;
        }
    }
    layer.z_order = max_z + 1;

    layers_[source_id] = layer;
    LOG_INFO("Added layer: " + source_id);
}

void Compositor::remove_layer(const std::string& source_id) {
    std::lock_guard<std::mutex> lock(layers_mutex_);

    auto it = layers_.find(source_id);
    if (it == layers_.end()) {
        LOG_WARNING("Layer not found: " + source_id);
        return;
    }

    layers_.erase(it);
    LOG_INFO("Removed layer: " + source_id);
}

void Compositor::update_layer_texture(const std::string& source_id, ID3D11Texture2D* texture) {
    std::lock_guard<std::mutex> lock(layers_mutex_);

    auto it = layers_.find(source_id);
    if (it == layers_.end()) {
        LOG_WARNING("Layer not found for texture update: " + source_id);
        return;
    }

    it->second.d3d_texture = texture;
    update();  // Trigger repaint
}

void Compositor::updateLayerImage(QString source_id, QImage image) {
    update_layer_image(source_id.toStdString(), image);
}

void Compositor::update_layer_image(const std::string& source_id, const QImage& image) {
    LOG_INFO("[DIAG] Compositor::update_layer_image for '" + source_id + "'. Image isNull: " + (image.isNull() ? "yes" : "no") + ", size: " + std::to_string(image.width()) + "x" + std::to_string(image.height()));
    std::lock_guard<std::mutex> lock(layers_mutex_);

    auto it = layers_.find(source_id);
    if (it == layers_.end()) {
        LOG_WARNING("Layer not found for image update: " + source_id);
        return;
    }

    it->second.qimage = image.copy(); // Use deep copy for cross-thread safety
    update();  // Trigger repaint
}

void Compositor::update_layer_transform(const std::string& source_id, const QRectF& dest_rect, float opacity) {
    std::lock_guard<std::mutex> lock(layers_mutex_);

    auto it = layers_.find(source_id);
    if (it == layers_.end()) {
        LOG_WARNING("Layer not found for transform update: " + source_id);
        return;
    }

    it->second.dest_rect = dest_rect;
    it->second.opacity = opacity;
    update();  // Trigger repaint
}

void Compositor::set_layer_visible(const std::string& source_id, bool visible) {
    std::lock_guard<std::mutex> lock(layers_mutex_);

    auto it = layers_.find(source_id);
    if (it == layers_.end()) {
        LOG_WARNING("Layer not found for visibility update: " + source_id);
        return;
    }

    it->second.visible = visible;
    update();  // Trigger repaint
}

std::vector<std::string> Compositor::get_layer_ids() const {
    std::lock_guard<std::mutex> lock(layers_mutex_);
    std::vector<std::string> ids;
    ids.reserve(layers_.size());
    for (const auto& kv : layers_) {
        ids.push_back(kv.first);
    }
    return ids;
}

bool Compositor::has_layer(const std::string& source_id) const {
    std::lock_guard<std::mutex> lock(layers_mutex_);
    return layers_.find(source_id) != layers_.end();
}

void Compositor::set_layer_order(const std::string& source_id, int order) {
    std::lock_guard<std::mutex> lock(layers_mutex_);

    auto it = layers_.find(source_id);
    if (it == layers_.end()) {
        LOG_WARNING("Layer not found for order update: " + source_id);
        return;
    }

    it->second.z_order = order;
    update();
}

QImage Compositor::render_to_image(int width, int height) {
    if (width <= 0 || height <= 0) {
        return {};
    }

    QImage img(width, height, QImage::Format_ARGB32);
    img.fill(Qt::black);

    QPainter painter(&img);

    // Keep consistent coordinate system with canvas
    const QSize canvas = canvas_size_;
    if (canvas.width() > 0 && canvas.height() > 0 && (canvas.width() != width || canvas.height() != height)) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.scale(static_cast<double>(width) / canvas.width(), static_cast<double>(height) / canvas.height());
    }

    render(&painter, QRect(0, 0, width, height));
    painter.end();

    return img;
}

void Compositor::set_canvas_size(int width, int height) {
    canvas_size_ = QSize(width, height);
    LOG_INFO("Canvas size set to: " + std::to_string(width) + "x" + std::to_string(height));
    update();
}

void Compositor::paintEvent(QPaintEvent* event) {
    LOG_INFO("[DIAG] 进入paintEvent方法");
    QPainter painter(this);

    // Fill background
    painter.fillRect(rect(), Qt::black);
    LOG_INFO("[DIAG] 绘制背景完成");

    // Draw layers
    std::lock_guard<std::mutex> lock(layers_mutex_);
    LOG_INFO("[DIAG] 开始绘制图层，当前图层数量: " + std::to_string(layers_.size()));

    std::vector<CompositorLayer> sorted_layers;
    for (const auto& pair : layers_) {
        sorted_layers.push_back(pair.second);
    }
    std::sort(sorted_layers.begin(), sorted_layers.end(), [](const auto& a, const auto& b) {
        return a.z_order < b.z_order;
    });
    LOG_INFO("[DIAG] 图层排序完成");

    for (const auto& layer : sorted_layers) {
        if (layer.visible) {
            LOG_INFO("[DIAG] 绘制图层: ID=" + layer.source_id + ", z_order=" + std::to_string(layer.z_order) + ", 可见性=true");
            painter.setOpacity(layer.opacity);

            // Draw QImage if available, otherwise draw placeholder
            if (!layer.qimage.isNull()) {
                LOG_INFO("[DIAG] 图层 " + layer.source_id + " 有图像数据, 尺寸: " + std::to_string(layer.qimage.width()) + "x" + std::to_string(layer.qimage.height()));
                painter.drawImage(layer.dest_rect.toRect(), layer.qimage);
                LOG_INFO("[DIAG] 图层 " + layer.source_id + " 绘制完成");
            } else {
                LOG_INFO("[DIAG] 图层 " + layer.source_id + " 没有图像数据, 绘制占位符");
                // Placeholder rectangle
                painter.fillRect(layer.dest_rect.toRect(), QColor(64, 128, 255));
                painter.setPen(Qt::white);
                painter.drawText(layer.dest_rect.toRect(), Qt::AlignCenter,
                               QString("Layer: %1").arg(QString::fromStdString(layer.source_id)));
                LOG_INFO("[DIAG] 图层 " + layer.source_id + " 占位符绘制完成");
            }
        } else {
            LOG_INFO("[DIAG] 图层: ID=" + layer.source_id + ", z_order=" + std::to_string(layer.z_order) + ", 可见性=false, 跳过绘制");
        }
    }
    LOG_INFO("[DIAG] 所有图层绘制完成");

    // Update performance stats
    perf_stats_.frame_count++;
    auto current_time = std::chrono::steady_clock::now();
    auto time_since_last_update = std::chrono::duration_cast<std::chrono::seconds>(
        current_time - perf_stats_.last_update);

    if (time_since_last_update.count() >= 1.0) {  // Update every second
        perf_stats_.avg_fps = static_cast<double>(perf_stats_.frame_count) / time_since_last_update.count();
        perf_stats_.frame_count = 0;
        perf_stats_.last_update = current_time;
        LOG_INFO("[DIAG] 性能统计更新: 平均帧率=" + std::to_string(perf_stats_.avg_fps) + " fps");
    }

    // Emit frame ready signal
    LOG_INFO("[DIAG] 发出frame_ready信号");
    emit frame_ready();
    LOG_INFO("[DIAG] paintEvent方法结束");
}

void Compositor::get_performance_stats(double& avg_fps, double& avg_render_time_ms, size_t& frame_count) const {
    avg_fps = perf_stats_.avg_fps;
    avg_render_time_ms = 0.0;  // Not measured in simple implementation
    frame_count = perf_stats_.frame_count;
}

void Compositor::render(QPainter* painter, const QRect& target_rect) {
    if (!painter) return;

    LOG_INFO("[DIAG] Compositor::render called, target_rect: " + std::to_string(target_rect.width()) + "x" + std::to_string(target_rect.height()));

    // Draw layers
    std::lock_guard<std::mutex> lock(layers_mutex_);

    // Create a sorted list of layers based on z_order before rendering
    std::vector<CompositorLayer> sorted_layers;
    sorted_layers.reserve(layers_.size());
    for (const auto& pair : layers_) {
        sorted_layers.push_back(pair.second);
    }
    std::sort(sorted_layers.begin(), sorted_layers.end(), [](const auto& a, const auto& b) {
        return a.z_order < b.z_order;
    });

    for (const auto& layer : sorted_layers) {
        if (layer.visible) {
            painter->setOpacity(layer.opacity);

            // Draw QImage if available, otherwise draw placeholder
            if (!layer.qimage.isNull()) {
                LOG_INFO("[DIAG] Compositor::render - 绘制图层 " + layer.source_id + ", 图像尺寸: " + std::to_string(layer.qimage.width()) + "x" + std::to_string(layer.qimage.height()) + ", 目标矩形: " + std::to_string(layer.dest_rect.width()) + "x" + std::to_string(layer.dest_rect.height()));

                // Use the layer's dest_rect for positioning and scaling
                QRectF target_rect_in_canvas = layer.dest_rect;

                // Scale to fit the dest_rect, maintaining aspect ratio
                QSize image_size = layer.qimage.size();
                QSizeF target_size = target_rect_in_canvas.size();

                // Scale to fill the target rect, cropping if necessary (like CanvasWidget)
                QImage scaled = layer.qimage.scaled(target_size.toSize(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);

                // Calculate source rect to crop the center part if needed
                QRectF source_rect;
                if (scaled.width() > target_size.width() || scaled.height() > target_size.height()) {
                    double source_x = (scaled.width() - target_size.width()) / 2.0;
                    double source_y = (scaled.height() - target_size.height()) / 2.0;
                    source_rect = QRectF(source_x, source_y, target_size.width(), target_size.height());
                } else {
                    source_rect = QRectF(0, 0, scaled.width(), scaled.height());
                }

                // Draw the image at the specified position
                painter->drawImage(target_rect_in_canvas, scaled, source_rect);

                LOG_INFO("[DIAG] Compositor::render - 图层 " + layer.source_id + " 绘制完成, 位置: (" + std::to_string(target_rect_in_canvas.x()) + "," + std::to_string(target_rect_in_canvas.y()) + "), 大小: " + std::to_string(target_rect_in_canvas.width()) + "x" + std::to_string(target_rect_in_canvas.height()));
            } else {
                // Placeholder rectangle using the layer's dest_rect
                painter->fillRect(layer.dest_rect, QColor(64, 128, 255));
                painter->setPen(Qt::white);
                painter->drawText(layer.dest_rect, Qt::AlignCenter,
                               QString("Layer: %1").arg(QString::fromStdString(layer.source_id)));
            }
        }
    }
}

void Compositor::reset_performance_stats() {
    perf_stats_.avg_fps = 0.0;
    perf_stats_.frame_count = 0;
    perf_stats_.last_update = std::chrono::steady_clock::now();
}

} // namespace live_assistant
