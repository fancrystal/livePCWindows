#include "scene_manager/compositor.h"
#include "scene_manager/shared_d3d_device.h"
#include "common/log.h"
#include "scene_manager/render_utils.h"
#include "video_engine/video_engine.h"  // For VideoFrame definition

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
    {
        std::lock_guard<std::mutex> lock(layers_mutex_);
        auto it = layers_.find(source_id);
        if (it == layers_.end()) {
            LOG_WARNING("Layer not found for texture update: " + source_id);
            return;
        }
        it->second.d3d_texture = texture;
    }
    update();  // Trigger repaint — called outside the lock to avoid deadlock with paintEvent
}

void Compositor::updateLayerImage(QString source_id, QImage image) {
    update_layer_image(source_id.toStdString(), image);
}

// Phase 1: GPU 纹理直传（线程安全，存储 GpuTextureRef，供 Phase 2 GPU 合成器使用）
void Compositor::update_layer_gpu_texture(const std::string& source_id, const GpuTextureRef& tex_ref) {
    std::lock_guard<std::mutex> lock(layers_mutex_);
    auto it = layers_.find(source_id);
    if (it == layers_.end()) return;
    it->second.gpu_texture_ref = tex_ref;
    // 注意：此处不调用 update()（不触发 QPainter 重绘）
    // Phase 2 的 GPU 合成器将直接读取 gpu_texture_ref，不通过 Qt 绘制事件
}

void Compositor::update_layer_video_frame(const std::string& source_id, std::shared_ptr<VideoFrame> frame) {
    {
        std::lock_guard<std::mutex> lock(layers_mutex_);
        auto it = layers_.find(source_id);
        if (it == layers_.end()) {
            LOG_WARNING("Layer not found for video frame update: " + source_id);
            return;
        }
        // 存储 shared_ptr<VideoFrame>，保持数据存活
        // 不做任何拷贝，零开销！
        it->second.video_frame = frame;
    }
    update();  // Trigger repaint — called outside the lock to avoid deadlock with paintEvent
}

void Compositor::update_layer_image(const std::string& source_id, const QImage& image) {
    {
        std::lock_guard<std::mutex> lock(layers_mutex_);
        auto it = layers_.find(source_id);
        if (it == layers_.end()) {
            LOG_WARNING("Layer not found for image update: " + source_id);
            return;
        }
        it->second.qimage = image;

        // Phase 1b: 同步上传到 GPU，供 Phase 2 GPU 合成器使用
        // 复用上次的纹理对象（尺寸不变时 Map_WRITE_DISCARD 不重分配）
        // 注意：使用 device()（而非 is_valid()）确保设备未初始化时能触发初始化，
        // 避免早期帧上传被跳过导致 gpu_texture_ref 空置、GPU 合成路径退化为 CPU 路径
        auto& shared = SharedD3D11Device::instance();
        if (shared.device() && !image.isNull()) {
            ID3D11Texture2D* reuse_tex = it->second.gpu_texture_ref.texture
                                             ? it->second.gpu_texture_ref.texture.get()
                                             : nullptr;
            auto new_tex = shared.upload_image_to_texture(image, reuse_tex);
            if (new_tex) {
                it->second.gpu_texture_ref.texture = new_tex;
                it->second.gpu_texture_ref.width   = static_cast<uint32_t>(image.width());
                it->second.gpu_texture_ref.height  = static_cast<uint32_t>(image.height());
                it->second.gpu_texture_ref.format  = DXGI_FORMAT_B8G8R8A8_UNORM;
            }
        }
    }
    update();  // Trigger repaint — called outside the lock to avoid deadlock with paintEvent
}

void Compositor::update_layer_transform(const std::string& source_id, const QRectF& dest_rect, float opacity) {
    {
        std::lock_guard<std::mutex> lock(layers_mutex_);
        auto it = layers_.find(source_id);
        if (it == layers_.end()) {
            LOG_WARNING("Layer not found for transform update: " + source_id);
            return;
        }
        it->second.dest_rect = dest_rect;
        it->second.opacity = opacity;
        LOG_INFO("Updated layer transform: " + source_id + " -> rect(" +
                 std::to_string(static_cast<int>(dest_rect.x())) + "," +
                 std::to_string(static_cast<int>(dest_rect.y())) + " " +
                 std::to_string(static_cast<int>(dest_rect.width())) + "x" +
                 std::to_string(static_cast<int>(dest_rect.height())) + ")");
    }
    update();  // Trigger repaint — called outside the lock to avoid deadlock with paintEvent
}

void Compositor::set_layer_visible(const std::string& source_id, bool visible) {
    {
        std::lock_guard<std::mutex> lock(layers_mutex_);
        auto it = layers_.find(source_id);
        if (it == layers_.end()) {
            LOG_WARNING("Layer not found for visibility update: " + source_id);
            return;
        }
        it->second.visible = visible;
    }
    update();  // Trigger repaint — called outside the lock to avoid deadlock with paintEvent
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
    {
        std::lock_guard<std::mutex> lock(layers_mutex_);
        auto it = layers_.find(source_id);
        if (it == layers_.end()) {
            LOG_WARNING("Layer not found for order update: " + source_id);
            return;
        }
        it->second.z_order = order;
    }
    update();  // Trigger repaint — called outside the lock to avoid deadlock with paintEvent
}

QImage Compositor::render_to_image(int width, int height) {
    if (width <= 0 || height <= 0) {
        return {};
    }

    // Use ARGB32 format to match WGC/BGRA output, avoiding format conversion
    // sws_scale can handle ARGB32 -> NV12 conversion directly
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

            // 优先使用 video_frame（零拷贝），否则使用 qimage（legacy兼容）
            QImage render_image;
            bool has_frame = false;

            if (layer.video_frame && layer.video_frame->data &&
                layer.video_frame->width > 0 && layer.video_frame->height > 0) {
                // 从 VideoFrame 创建临时 QImage（零拷贝，只引用数据）
                // 数据由 shared_ptr<VideoFrame> 保持存活，安全
                render_image = QImage(
                    layer.video_frame->data.get(),
                    layer.video_frame->width,
                    layer.video_frame->height,
                    layer.video_frame->stride,
                    QImage::Format_RGBA8888
                );
                has_frame = true;
            } else if (!layer.qimage.isNull()) {
                // Legacy 回退：使用 qimage
                render_image = layer.qimage;
                has_frame = true;
            }

            if (has_frame) {
                // Map layer.dest_rect (canvas coordinates) to widget coordinates
                QRect widget_rect = rect();
                auto mapping = compute_canvas_mapping(widget_rect, canvas_size_.width(), canvas_size_.height());
                double scale = mapping.scale;
                double offset_x = mapping.offset_x;
                double offset_y = mapping.offset_y;

                QRectF dest = layer.dest_rect;

                // 安全检查：确保目标矩形有效
                if (dest.width() <= 0 || dest.height() <= 0 ||
                    dest.x() < -dest.width() || dest.y() < -dest.height()) {
                    continue;
                }

                QRectF mapped_rect(dest.x() * scale + offset_x, dest.y() * scale + offset_y, dest.width() * scale, dest.height() * scale);

                painter.drawImage(mapped_rect, render_image);
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

            // 优先使用 video_frame（零拷贝），否则使用 qimage（legacy兼容）
            QImage render_image;
            bool has_frame = false;

            if (layer.video_frame && layer.video_frame->data) {
                // 从 VideoFrame 创建临时 QImage（零拷贝，只引用数据）
                // 数据由 shared_ptr<VideoFrame> 保持存活，安全
                render_image = QImage(
                    layer.video_frame->data.get(),
                    layer.video_frame->width,
                    layer.video_frame->height,
                    layer.video_frame->stride,
                    QImage::Format_RGBA8888
                );
                has_frame = true;
            } else if (!layer.qimage.isNull()) {
                // Legacy 回退：使用 qimage
                render_image = layer.qimage;
                has_frame = true;
            }

            if (has_frame) {
                // Use the layer's dest_rect for positioning and scaling
                QRectF target_rect_in_canvas = layer.dest_rect;

                QSizeF target_size = target_rect_in_canvas.size();
                QImage scaled = render_image.scaled(target_size.toSize(), Qt::KeepAspectRatio, Qt::FastTransformation);
                QPointF top_left(
                    target_rect_in_canvas.x() + (target_rect_in_canvas.width() - scaled.width()) / 2.0,
                    target_rect_in_canvas.y() + (target_rect_in_canvas.height() - scaled.height()) / 2.0);
                painter->drawImage(top_left, scaled);
            } else {
                // No frame available — fill with black (neutral no-signal color)
                // Previously used QColor(64, 128, 255) which caused confusing blue streams
                // when WGC returns E_ACCESSDENIED (e.g. display protected by WDA_EXCLUDEFROMCAPTURE)
                painter->fillRect(layer.dest_rect, Qt::black);
            }
        }
    }
}

void Compositor::reset_performance_stats() {
    perf_stats_.avg_fps = 0.0;
    perf_stats_.frame_count = 0;
    perf_stats_.last_update = std::chrono::steady_clock::now();
}

std::optional<CompositorLayer> Compositor::get_layer_state(const std::string& source_id) const {
    std::lock_guard<std::mutex> lock(layers_mutex_);

    auto it = layers_.find(source_id);
    if (it == layers_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<CompositorLayer> Compositor::get_all_layers() const {
    std::lock_guard<std::mutex> lock(layers_mutex_);
    std::vector<CompositorLayer> result;
    result.reserve(layers_.size());
    for (const auto& [id, layer] : layers_) {
        result.push_back(layer);
    }
    return result;
}

void Compositor::move_layer_up(const std::string& source_id) {
    std::lock_guard<std::mutex> lock(layers_mutex_);

    auto it = layers_.find(source_id);
    if (it == layers_.end()) {
        LOG_WARNING("Layer not found for move up: " + source_id);
        return;
    }

    // Find the highest z_order
    int max_z = it->second.z_order;
    for (const auto& pair : layers_) {
        if (pair.second.z_order > max_z) {
            max_z = pair.second.z_order;
        }
    }

    // If not already at top, swap with layer above
    if (it->second.z_order < max_z) {
        // Find the layer just above this one
        for (auto& pair : layers_) {
            if (pair.second.z_order == it->second.z_order + 1) {
                pair.second.z_order--;
                break;
            }
        }
        it->second.z_order++;
    }
    update();
}

void Compositor::move_layer_down(const std::string& source_id) {
    std::lock_guard<std::mutex> lock(layers_mutex_);

    auto it = layers_.find(source_id);
    if (it == layers_.end()) {
        LOG_WARNING("Layer not found for move down: " + source_id);
        return;
    }

    // If not already at bottom, swap with layer below
    if (it->second.z_order > 0) {
        // Find the layer just below this one
        for (auto& pair : layers_) {
            if (pair.second.z_order == it->second.z_order - 1) {
                pair.second.z_order++;
                break;
            }
        }
        it->second.z_order--;
    }
    update();
}

} // namespace live_assistant
