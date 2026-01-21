#pragma once

#include <QMainWindow>
#include <QPushButton>
#include <QListWidget>
#include <QStyle>
#include <QTimer>
#include <memory>
#include <qlabel.h>

#include "common/media_clock.h"
#include "common/config_manager.h"

namespace Ui {
class MainWindow;
}

namespace live_assistant {

// Forward declarations
class SceneManager;
class VideoEngine;
class AudioEngine;
class Encoder;
class StreamPusher;
class CanvasWidget;
class Compositor;
class CompositorEncoderBridge;
class CaptureManagerIface;
class Source;
class SceneItem;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    void set_live_id(const QString& live_id);

    // 推流目标（后续与直播间列表联通时由外部设置）
    void set_rtmp_target(const QString& server_url, const QString& stream_key);

    // 画布配置管理
    void set_canvas_config(const CanvasConfig& config);
    const CanvasConfig& get_canvas_config() const;

private slots:
    void update_preview();
    void encode_and_push();
    void on_camera_button_clicked();
    void on_select_camera(const QString& camera_name, const std::string& camera_device_id);
    void on_camera_frame_ready();
    void on_screen_share_button_clicked();
    void on_select_screen_share(const QString& target_name, bool is_screen_mode, int fps, const QString& resolution, bool capture_cursor, bool capture_border);
    void on_scene_item_reordered();

    // 推流控制
    void on_streaming_started();
    void on_streaming_stopped();
    void on_streaming_error(const QString& error);

private:
    void setup_scene_list();
    void build_scene_list();

    QListWidget* listWidget_sceneItems_{nullptr};

    Ui::MainWindow *ui;

    // Modules
    std::shared_ptr<SceneManager> scene_manager_;
    std::shared_ptr<VideoEngine> video_engine_;
    std::shared_ptr<AudioEngine> audio_engine_;
    std::shared_ptr<Encoder> encoder_;
    std::shared_ptr<StreamPusher> stream_pusher_;
    std::shared_ptr<CaptureManagerIface> capture_manager_;

    // Canvas widget
    CanvasWidget* canvas_widget_;

    // Compositor and encoder bridge
    std::shared_ptr<Compositor> compositor_;
    std::shared_ptr<CompositorEncoderBridge> encoder_bridge_;

    // Preview timer
    QTimer* preview_timer_;

    // Encoding timer for pushing stream（这里主要用于音频推送；视频由 CompositorEncoderBridge 推送）
    QTimer* encoding_timer_;

    // Media clock for timestamp synchronization
    MediaClock media_clock_;

    // Canvas configuration
    CanvasConfig canvas_config_ = CanvasConfig::get_default();

    // Live ID
    QString live_id_;

    // Camera related
    bool is_camera_preview_ = false;

    // Stream registration state
    bool streams_registered_ = false;

    // RTMP target
    QString rtmp_server_url_;
    QString rtmp_stream_key_;

    // Live duration display
    QLabel* live_duration_label_ = nullptr;
    QTimer* live_duration_timer_ = nullptr;
    qint64 streaming_start_time_ms_ = 0;

    // Initialization
    void initialize_modules();
    void setup_ui_connections();
    void update_status(const QString& message);
    void setup_canvas_widget();

    // A-mode: Scene is authoritative; keep Compositor layers in sync with SceneItems.
    void sync_scene_to_compositor();

    // Camera methods
    void start_camera_preview();
    void stop_camera_preview();
    void show_camera_selector();

    // Screen share methods
    void show_screen_share_selector();

    // Scene item management methods
    void toggle_scene_item_visibility(int index);
    void show_scene_item_settings(int index);
    void delete_scene_item(int index);

    // Scene items UI management
    void update_scene_items();

    // Helper methods for UI components
    QString extract_source_name(std::shared_ptr<Source> source);
    QPushButton* create_scene_item_button(const QString& text, const QString& style = "");
    QPushButton* create_icon_button(const QString& icon_text, const QString& tooltip = "");
    QPushButton* create_icon_button(QStyle::StandardPixmap icon, const QString& tooltip = "");
};

} // namespace live_assistant
