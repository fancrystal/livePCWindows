#include "scene_manager/compositor.h"
#include "common/log.h"

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

void Compositor::update_layer_image(const std::string& source_id, const QImage& image) {
    std::lock_guard<std::mutex> lock(layers_mutex_);

    auto it = layers_.find(source_id);
    if (it == layers_.end()) {
        LOG_WARNING("Layer not found for image update: " + source_id);
        return;
    }

    it->second.qimage = image;
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

bool Compositor::has_layer(const std::string& source_id) const {
    std::lock_guard<std::mutex> lock(layers_mutex_);
    return layers_.find(source_id) != layers_.end();
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
    for (const auto& pair : layers_) {
        const auto& layer = pair.second;
        if (layer.visible) {
            painter.setOpacity(layer.opacity);

            // Draw QImage if available, otherwise draw placeholder
            if (!layer.qimage.isNull()) {
                painter.drawImage(layer.dest_rect.toRect(), layer.qimage);
            } else {
                // Placeholder rectangle
                painter.fillRect(layer.dest_rect.toRect(), QColor(64, 128, 255));
                painter.setPen(Qt::white);
                painter.drawText(layer.dest_rect.toRect(), Qt::AlignCenter,
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
    for (const auto& pair : layers_) {
        const auto& layer = pair.second;
        if (layer.visible) {
            painter->setOpacity(layer.opacity);

            // Draw QImage if available, otherwise draw placeholder
            if (!layer.qimage.isNull()) {
                painter->drawImage(layer.dest_rect.toRect(), layer.qimage);
            } else {
                // Placeholder rectangle
                painter->fillRect(layer.dest_rect.toRect(), QColor(64, 128, 255));
                painter->setPen(Qt::white);
                painter->drawText(layer.dest_rect.toRect(), Qt::AlignCenter,
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