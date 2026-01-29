#pragma once

#include <QDialog>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QScrollArea>
#include <QEvent>
#include <vector>
#include <memory>
#include <chrono>
#include <QImage>

namespace live_assistant {

struct CaptureTarget {
    enum class Type { SCREEN, WINDOW };

    Type type;
    std::string id;           // Unique identifier (monitor device name or window handle)
    std::string name;         // Display name
    QPixmap thumbnail;        // Low-res thumbnail for list view
    QSize size;               // Original size
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
    void on_ok_clicked();
    void on_cancel_clicked();

private:
    void setup_ui();
    void enumerate_screens();
    void enumerate_windows();
    void create_thumbnail_for_screen(CaptureTarget& target);
    void create_thumbnail_for_window(CaptureTarget& target);
    QWidget* create_thumbnail_widget(const CaptureTarget& target, int width, int height);
    QPixmap create_window_thumbnail(HWND hwnd, int width, int height);
    QPixmap create_screen_thumbnail(const QString& device_name, int thumb_width, int thumb_height);
    void on_thumbnail_clicked(int index);
    bool eventFilter(QObject* obj, QEvent* event) override;

    QPushButton* ok_button_;
    QPushButton* cancel_button_;
    QPushButton* refresh_button_;

    QWidget* thumbnail_container_;
    QHBoxLayout* thumbnail_layout_;

    std::vector<CaptureTarget> targets_;
    CaptureTarget* selected_target_ = nullptr;
};

} // namespace live_assistant
