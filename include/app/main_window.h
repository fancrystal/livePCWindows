#pragma once

#include <QMainWindow>
#include <QPushButton>
#include <QListWidget>
#include <QStyle>
#include <QTimer>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <memory>
#include <qlabel.h>
#include <QPointer>
#include <atomic>

#include "common/media_clock.h"
#include "common/config_manager.h"
#include "common/system_monitor.h"
#include "http/live_item.h"
#include "media_pipeline/media_file_source.h"

class ExitDialog;
class InsertVideoWidget;

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
class SettingsPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT

signals:
    // 用户请求返回直播列表（离开当前直播间）
    void request_return_to_live_list();

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    void set_live_id(const QString& live_id);

    // 设置直播间凭证信息
    void setCredentials(const QString& socketUrl, const QString& userId, const QString& token, const QString& liveurl, const QString& oncekey);

    // 接收直播项（含插播文件）
    void setLiveItem(const LiveItem& liveItem);

    // 推流目标（后续与直播间列表联通时由外部设置）
    void set_rtmp_target(const QString& server_url, const QString& stream_key);

    // 画布配置管理
    void set_canvas_config(const CanvasConfig& config);
    const CanvasConfig& get_canvas_config() const;

    // 画布方向切换（横屏/竖屏）
    void set_landscape_mode();   // 设置为横屏 16:9 (1920x1080)
    void set_portrait_mode();    // 设置为竖屏 9:16 (1080x1920)
    void toggle_canvas_orientation();  // 切换横竖屏

    // 根据服务器配置设置画布方向（预留接口）
    void apply_server_canvas_config(const QString& orientation);

private slots:
    void update_preview();
    void encode_and_push();
    void on_camera_button_clicked();
    void on_select_camera(const QString& camera_name, const std::string& camera_device_id);
    void on_camera_frame_ready();
    void on_screen_share_button_clicked();
    void on_select_screen_share(const QString& target_name, bool is_screen_mode, int fps, const QString& resolution, bool capture_cursor, bool capture_border);
    void on_scene_item_reordered();

    // 插播视频功能
    void on_insert_video_button_clicked();
    void on_start_insert_video(const QString& fileId, const QString& fileName, bool loopEnabled);
    void on_stop_insert_video();
    void on_insert_video_frame_ready();  // 新增：处理同步器中的插播视频帧

    // 推流控制
    void on_streaming_started();
    void on_streaming_stopped();
    void on_streaming_error(const QString& error);

private:
    void setup_scene_list();
    void build_scene_list();
    void update_audio_status(const QString& text, const QString& color);
    void update_system_info();
    void log_system_stats_periodically();  // 新增：定期打印系统统计日志
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
    QWidget* canvasContainer_ = nullptr;  // 画布容器，用于保持宽高比
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
    bool is_portrait_mode_ = false;  // 当前是否为竖屏模式

    // 服务器配置字段（预留）
    QString server_canvas_orientation_;  // 服务器返回的画布方向: "landscape" | "portrait"

    // Live ID
    QString live_id_;

    // 直播间凭证信息
    QString socket_url_;
    QString user_id_;
    QString token_;
    QString live_url_;
    QString once_key_;
    QString domain_;  // 从socket_url提取的域名

    // 当前直播项信息
    LiveItem current_live_item_;

    // 插播视频相关
    InsertVideoWidget* insert_video_widget_ = nullptr;
    std::shared_ptr<MediaFileSource> current_insert_video_source_;  // 改为具体类型以便调用
    QString current_insert_video_file_id_;
    bool is_insert_video_playing_ = false;
    QTimer* insert_video_timer_ = nullptr;  // 插播视频帧同步定时器

    // Canvas orientation switch guards (prevent rapid toggles & re-entrancy)
    std::atomic_bool canvas_config_changing_{false};
    int64_t last_canvas_toggle_ms_ = 0;

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
    QTimer* system_info_timer_ = nullptr;
    // 系统监控日志打印定时器（每分钟打印一次）
    QTimer* system_log_timer_ = nullptr;

    // Tech stats label (bottom bar)
    QLabel* tech_stats_label_ = nullptr;

    // 系统监控日志打印计数器
    int system_log_counter_ = 0;

    // Audio control state
    bool microphone_enabled_ = true;
    bool speaker_enabled_ = true;

    // System tray
    QSystemTrayIcon* system_tray_icon_ = nullptr;
    QMenu* system_tray_menu_ = nullptr;
    QAction* tray_action_show_ = nullptr;
    QAction* tray_action_exit_ = nullptr;
    bool tray_icon_initializing_ = false;  // 防止托盘图标初始化时误触发显示窗口

    // Exit dialog preference (0: ask, 1: minimize, 2: exit)
    int exit_preference_ = 0;
    bool is_exiting_ = false;

    // Initialization
    void initialize_modules();
    void setup_ui_connections();
    void update_status(const QString& message);
    void setup_canvas_widget();
    void setupTitleBarButtons();
    void setupBottomButtonsStyle();
    void initWebEngineUI();     // 初始化WebView UI属性
    void initWebEngineViews();  // 初始化WebView控件（加载URL）

    // System tray methods
    void setupSystemTray();
    void setupNetworkConnections();
    void cleanupSystemTray();
    void onTrayIconActivated(QSystemTrayIcon::ActivationReason reason);
    void onTrayShowAction();
    void onTrayExitAction();

    // Exit handling
    void handleExit();
    void loadExitPreference();
    void saveExitPreference(int preference);

    // A-mode: Scene is authoritative; keep Compositor layers in sync with SceneItems.
    void sync_scene_to_compositor();

    // Camera methods
    void start_camera_preview();
    void stop_camera_preview();
    void show_camera_selector();

    // Screen share methods
    void show_screen_share_selector();

    // 插播视频方法
    void show_insert_video_widget();
    void startInsertVideoPlayback(const QString& fileId, bool loopEnabled);
    void stopInsertVideoPlayback();

    // Scene item management methods
    void toggle_scene_item_visibility(int index);
    void show_scene_item_settings(int index);
    void delete_scene_item(int index);

    // Apply SettingsPanel changes helper
    void applySettingsPanelChanges(SettingsPanel& dlg);

    // Audio control methods
    void toggle_microphone();
    void set_microphone_volume(float volume);
    void update_microphone_ui();
    void show_microphone_menu(const QPoint& pos);

    void toggle_speaker();
    void set_speaker_volume(float volume);
    void update_speaker_ui();
    void show_speaker_menu(const QPoint& pos);

    // Audio volume settings persistence
    void saveAudioVolumeSettings();
    void loadAudioVolumeSettings();
    void playVolumeFeedbackSound();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override; // 处理窗口状态变化
    void closeEvent(QCloseEvent* event) override; // 处理窗口关闭事件

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

    // 画布方向切换辅助方法
    void apply_canvas_config_change();           // 应用画布配置变更
    void adjust_scene_items_for_canvas_change(); // 调整场景项适配新画布
    void update_canvas_orientation_ui();         // 更新画布方向 UI
    void update_stage_container_aspect_ratio();  // 根据画布方向调整舞台容器宽高比
};

} // namespace live_assistant
