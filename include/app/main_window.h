#pragma once

#include <QMainWindow>
#include <QPushButton>
#include <QListWidget>
#include <QStyle>
#include <QTimer>
#include <memory>
#include <qlabel.h>
#include <QPointer>

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
    void update_audio_status(const QString& text, const QString& color);
    void update_streaming_stats();
    void update_system_info();
    std::pair<double, double> get_system_info();
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
    QPointer<CanvasWidget> canvas_widget_;
    // Stage placeholder container (visual only until canvas is attached)
    QPointer<QWidget> stageContainer_ = nullptr;
    QPointer<QPushButton> stageAddButton_ = nullptr;
    QPointer<QLabel> placeholderIcon_ = nullptr;
    QPointer<QLabel> placeholderText_ = nullptr;
    QPointer<QWidget> stagePlaceholderWidget_ = nullptr; // layout placeholder to reserve space

    // Stage drag / window state
    bool stage_dragging_ = false;
    QPoint stage_drag_start_pos_;
    QPoint stage_orig_pos_;
    bool stage_maximized_ = false;
    QRect stage_normal_geometry_;
    bool stage_minimized_ = false;
    QPushButton* stageRestoreButton_ = nullptr; // shown when minimized
    QPushButton* stageBtnMax_ = nullptr;
    QPushButton* stageBtnMin_ = nullptr;
    QPushButton* stageBtnRestore_ = nullptr;
    // Window dragging via custom title bar
    bool window_dragging_ = false;
    QPoint window_drag_start_pos_;

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
    QTimer* live_duration_timer_ = nullptr;
    qint64 streaming_start_time_ms_ = 0;

    // Statistics display
    QTimer* stats_update_timer_ = nullptr;
    QTimer* system_info_timer_ = nullptr;
    double cached_cpu_usage_ = 0.0;
    double cached_memory_mb_ = 0.0;
    // CPU sampling state (windows)
    uint64_t prev_idle_ = 0;
    uint64_t prev_sys_ = 0;
    bool first_cpu_sample_ = true;

    // Tech stats label (bottom bar)
    QLabel* tech_stats_label_ = nullptr;

    // Audio control state
    bool microphone_enabled_ = true;
    bool speaker_enabled_ = true;

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

    // Audio control methods
    void toggle_microphone();
    void set_microphone_volume(float volume);
    void update_microphone_ui();
    void show_microphone_menu(const QPoint& pos);

    void toggle_speaker();
    void set_speaker_volume(float volume);
    void update_speaker_ui();
    void show_speaker_menu(const QPoint& pos);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

    // Scene items UI management
    void update_scene_items();

    // Helper methods for UI components
    QString extract_source_name(std::shared_ptr<Source> source);
    QPushButton* create_scene_item_button(const QString& text, const QString& style = "");
    QPushButton* create_icon_button(const QString& icon_text, const QString& tooltip = "");
    QPushButton* create_icon_button(QStyle::StandardPixmap icon, const QString& tooltip = "");
    // UI helpers
    void updateStagePlaceholderVisibility();
    void repositionPlaceholderOverlays();
    // Stage window controls
    void toggleStageMaximize();
    void restoreStage();
};

} // namespace live_assistant
