#pragma once

#include <QRect>
#include <QRectF>
#include <algorithm>

namespace live_assistant {

// Utility structure for canvas-to-widget mapping calculations
struct CanvasMapping {
    double scale;
    double offset_x;
    double offset_y;
};

// Compute scaling and offset for mapping canvas coordinates to widget coordinates
inline CanvasMapping compute_canvas_mapping(const QRect& widget_rect, int canvas_width, int canvas_height) {
    CanvasMapping mapping;

    int base_w = canvas_width > 0 ? canvas_width : 1920;
    int base_h = canvas_height > 0 ? canvas_height : 1080;

    double scale_x = static_cast<double>(widget_rect.width()) / static_cast<double>(base_w);
    double scale_y = static_cast<double>(widget_rect.height()) / static_cast<double>(base_h);
    mapping.scale = (std::min)(scale_x, scale_y); // Keep aspect ratio

    // Calculate centering offsets
    mapping.offset_x = (widget_rect.width() - base_w * mapping.scale) / 2.0;
    mapping.offset_y = (widget_rect.height() - base_h * mapping.scale) / 2.0;

    return mapping;
}

// Map canvas coordinates to widget coordinates
inline QRect map_canvas_to_widget(const QRectF& canvas_rect, const CanvasMapping& mapping) {
    int x = static_cast<int>(canvas_rect.x() * mapping.scale + mapping.offset_x);
    int y = static_cast<int>(canvas_rect.y() * mapping.scale + mapping.offset_y);
    int width = static_cast<int>(canvas_rect.width() * mapping.scale);
    int height = static_cast<int>(canvas_rect.height() * mapping.scale);

    return QRect(x, y, width, height);
}

// Convert widget deltas to canvas deltas (inverse of canvas-to-widget mapping)
inline void convert_widget_deltas_to_canvas(int widget_dx, int widget_dy, int& canvas_dx, int& canvas_dy, const CanvasMapping& mapping) {
    canvas_dx = widget_dx;
    canvas_dy = widget_dy;
    if (mapping.scale > 1e-6) {
        canvas_dx = static_cast<int>(std::round(static_cast<double>(widget_dx) / mapping.scale));
        canvas_dy = static_cast<int>(std::round(static_cast<double>(widget_dy) / mapping.scale));
    }
}

} // namespace live_assistant

