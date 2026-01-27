#include "scene_manager/compositor.h"
#include "common/log.h"
#include "scene_manager/render_utils.h"

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

    // For streaming/encoding, we want to render the full canvas content at the target resolution
    // Don't apply scaling if canvas size matches the target size (streaming case)
    const QSize canvas = canvas_size_;
    if (canvas.width() > 0 && canvas.height() > 0 && (canvas.width() != width || canvas.height() != height)) {
        // This is for preview rendering where canvas size differs from display size
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.scale(static_cast<double>(width) / canvas.width(), static_cast<double>(height) / canvas.height());
    }
    // For streaming (canvas_size == target_size), render directly without scaling

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
    QPainter painter(this);

    // Fill background
    painter.fillRect(rect(), Qt::black);

    // Draw layers
    std::lock_guard<std::mutex> lock(layers_mutex_);

    std::vector<CompositorLayer> sorted_layers;
    for (const auto& pair : layers_) {
        sorted_layers.push_back(pair.second);
    }
    std::sort(sorted_layers.begin(), sorted_layers.end(), [](const auto& a, const auto& b) {
        return a.z_order < b.z_order;
    });

    for (const auto& layer : sorted_layers) {
        if (layer.visible) {
            painter.setOpacity(layer.opacity);

            // Draw QImage if available, otherwise draw placeholder
            if (!layer.qimage.isNull()) {
                // Map layer.dest_rect (canvas coordinates) to widget coordinates
                QRect widget_rect = rect();
                auto mapping = compute_canvas_mapping(widget_rect, canvas_size_.width(), canvas_size_.height());
                double scale = mapping.scale;
                double offset_x = mapping.offset_x;
                double offset_y = mapping.offset_y;

                QRectF dest = layer.dest_rect;
                QRectF mapped_rect(dest.x() * scale + offset_x, dest.y() * scale + offset_y, dest.width() * scale, dest.height() * scale);

                painter.drawImage(mapped_rect, layer.qimage);
            } else {
                // Placeholder rectangle
                QRect widget_rect = rect();
                auto mapping = compute_canvas_mapping(widget_rect, canvas_size_.width(), canvas_size_.height());
                double scale = mapping.scale;
                double offset_x = mapping.offset_x;
                double offset_y = mapping.offset_y;
                QRectF dest = layer.dest_rect;
                QRect mapped_rect(static_cast<int>(dest.x() * scale + offset_x),
                                  static_cast<int>(dest.y() * scale + offset_y),
                                  static_cast<int>(dest.width() * scale),
                                  static_cast<int>(dest.height() * scale));
                painter.fillRect(mapped_rect, QColor(64, 128, 255));
                painter.setPen(Qt::white);
                painter.drawText(mapped_rect, Qt::AlignCenter,
                               QString("Layer: %1").arg(QString::fromStdString(layer.source_id)));
            }
        }
    }

    // Update performance stats
    perf_stats_.frame_count++;
    auto current_time = std::chrono::steady_clock::now();
    auto time_since_last_update = std::chrono::duration_cast<std::chrono::seconds>(
        current_time - perf_stats_.last_update);

    if (time_since_last_update.count() >= 1.0) {  // Update every second
        perf_stats_.avg_fps = static_cast<double>(perf_stats_.frame_count) / time_since_last_update.count();
        perf_stats_.frame_count = 0;
        perf_stats_.last_update = current_time;
    }

    // Emit frame ready signal
    emit frame_ready();
}

void Compositor::get_performance_stats(double& avg_fps, double& avg_render_time_ms, size_t& frame_count) const {
    avg_fps = perf_stats_.avg_fps;
    avg_render_time_ms = 0.0;  // Not measured in simple implementation
    frame_count = perf_stats_.frame_count;
}

void Compositor::render(QPainter* painter, const QRect& target_rect) {
    if (!painter) return;

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
