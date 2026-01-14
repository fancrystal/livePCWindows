#pragma once

#include <QDialog>
#include <QListWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <vector>
#include <memory>
#include <chrono>
#include <QImage>
#include <QFutureWatcher>
#include <QMutex>

namespace live_assistant {

struct CaptureTarget {
    enum class Type { SCREEN, WINDOW };

    Type type;
    std::string id;           // Unique identifier (monitor device name or window handle)
    std::string name;         // Display name
    QPixmap thumbnail;        // Low-res thumbnail for list view
    QPixmap hd_preview;       // High-res preview, generated on-demand
    QSize size;               // Original size
    std::chrono::steady_clock::time_point last_update = std::chrono::steady_clock::now();
    bool updating = false;
};

class ScreenCaptureSelector : public QDialog {
    Q_OBJECT

public:
    explicit ScreenCaptureSelector(QWidget *parent = nullptr);
    ~ScreenCaptureSelector();

    // Get selected target
    const CaptureTarget* get_selected_target() const;

private slots:
    void refresh_targets();
    void on_item_selection_changed();
    void on_ok_clicked();
    void on_cancel_clicked();
    void update_thumbnails();
    void start_async_thumbnail_update(int index);

private:
    void setup_ui();
    void enumerate_screens();
    void enumerate_windows();
    void create_thumbnail_for_screen(CaptureTarget& target);
    void create_thumbnail_for_window(CaptureTarget& target);
    QPixmap create_window_thumbnail(HWND hwnd, int width, int height);
    QPixmap create_screen_thumbnail(const QString& device_name, int thumb_width, int thumb_height);

    QListWidget* list_widget_;
    QLabel* preview_label_;
    QPushButton* ok_button_;
    QPushButton* cancel_button_;
    QPushButton* refresh_button_;

    QTimer* update_timer_;

    std::vector<CaptureTarget> targets_;
    CaptureTarget* selected_target_ = nullptr;
    // No persistent watchers: async updates use detached std::thread and queued UI updates
};

} // namespace live_assistant