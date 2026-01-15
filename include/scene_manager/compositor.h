#pragma once

#include <QWidget>
#include <QPainter>
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <d3d11.h>
#include <dxgi1_2.h>

namespace live_assistant {

struct CompositorLayer {
    std::string source_id;
    ID3D11Texture2D* d3d_texture = nullptr;
    QImage qimage;  // For CPU-based rendering
    QRectF dest_rect;  // Destination rectangle in canvas coordinates
    float opacity = 1.0f;
    bool visible = true;
    int z_order = 0;
};

class Compositor : public QWidget {
    Q_OBJECT

public:
    explicit Compositor(QWidget* parent = nullptr);
    ~Compositor() override;

    // Layer management
    void add_layer(const std::string& source_id);
    void remove_layer(const std::string& source_id);
    void update_layer_texture(const std::string& source_id, ID3D11Texture2D* texture);
    void update_layer_image(const std::string& source_id, const QImage& image);
    void update_layer_transform(const std::string& source_id, const QRectF& dest_rect, float opacity = 1.0f);
    void set_layer_visible(const std::string& source_id, bool visible);
    bool has_layer(const std::string& source_id) const;

    // Layer ordering and state access
    std::vector<std::string> get_layer_ids() const;
    std::optional<CompositorLayer> get_layer_state(const std::string& source_id) const;
    void move_layer_up(const std::string& source_id);
    void move_layer_down(const std::string& source_id);

    // Canvas properties
    void set_canvas_size(int width, int height);
    QSize get_canvas_size() const { return canvas_size_; }

    // For now, return 0 since we don't use OpenGL texture
    unsigned int get_output_texture() const { return 0; }

    // Render to a QPainter (for CPU-based rendering)
    void render(QPainter* painter, const QRect& target_rect);

    // Performance monitoring
    void get_performance_stats(double& avg_fps, double& avg_render_time_ms, size_t& frame_count) const;
    void reset_performance_stats();

signals:
    void frame_ready();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    // Layer data
    mutable std::mutex layers_mutex_;
    std::unordered_map<std::string, CompositorLayer> layers_;

    // Canvas properties
    QSize canvas_size_;

    // Performance monitoring
    struct PerformanceStats {
        double avg_fps = 0.0;
        size_t frame_count = 0;
        std::chrono::steady_clock::time_point last_update;
    };
    PerformanceStats perf_stats_;
};

} // namespace live_assistant