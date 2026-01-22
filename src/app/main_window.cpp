#include "app/main_window.h"
#include "app/camera_settings.h"
#include "app/screen_share.h"
#include "app/screen_capture_selector.h"
#include "app/settings_dialog.h"
#include "ui_main_window.h"
#include "scene_manager/scene_manager.h"
#include "scene_manager/source_factory.h"
#include "scene_manager/canvas.h"
#include "scene_manager/compositor.h"
#include "scene_manager/compositor_encoder_bridge.h"
#include "video_engine/video_engine.h"
#include "audio_engine/audio_engine.h"
#include "scene_manager/wgc_capture_stub.h"
#include "scene_manager/capture_factory.h"
#include "scene_manager/capture_manager_iface.h"
#include "audio_engine/audio_engine.h"
#include "encoder/encoder.h"
#include "stream_pusher/stream_pusher.h"
#include "common/log.h"
#include "common/error.h"

#include <QTimer>
#include <QImage>
#include <QPixmap>
#include <QMessageBox>
#include <QLabel>
#include <QDateTime>
#include <QInputDialog>
#include <QHBoxLayout>
#include <QMenu>
#include <QAction>
#include <QStyle>
#include <QAbstractItemModel>
#include <QVariant>

#include <unordered_set>
#include <algorithm>

#include "app/add_material_dialog.h"
#include "app/scene_item_row.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace live_assistant {

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow) {
    ui->setupUi(this);

    setWindowTitle("LiveAssistant");

    preview_timer_ = new QTimer(this);
    connect(preview_timer_, &QTimer::timeout, this, &MainWindow::update_preview);

    setup_ui_connections();
    setup_scene_list();

    update_status("Ready");

    LOG_INFO("MainWindow created");
}

MainWindow::~MainWindow() {
    LOG_INFO("MainWindow destroyed");
    delete ui;
}

void MainWindow::setup_scene_list() {
    if (listWidget_sceneItems_) return;

    listWidget_sceneItems_ = new QListWidget(this);
    listWidget_sceneItems_->setObjectName("listWidget_sceneItems");
    listWidget_sceneItems_->setDragDropMode(QAbstractItemView::InternalMove);
    listWidget_sceneItems_->setDefaultDropAction(Qt::MoveAction);
    listWidget_sceneItems_->setSelectionMode(QAbstractItemView::SingleSelection);
    listWidget_sceneItems_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    listWidget_sceneItems_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    listWidget_sceneItems_->setStyleSheet(
        "QListWidget { border: none; background: transparent; }\n"
        "QListWidget::item { padding: 2px; }\n"
        "QListWidget::item:selected { background: #2b2b2b; border-radius: 4px; }"
    );

    if (ui && ui->verticalLayout_sceneItems) {
        ui->verticalLayout_sceneItems->addWidget(listWidget_sceneItems_);
    }

    connect(listWidget_sceneItems_->model(), &QAbstractItemModel::rowsMoved,
            this, &MainWindow::on_scene_item_reordered);
}

void MainWindow::build_scene_list() {
    if (!listWidget_sceneItems_) return;
    if (!scene_manager_ || !scene_manager_->get_current_scene()) {
        listWidget_sceneItems_->clear();
        return;
    }

    listWidget_sceneItems_->clear();

    auto scene = scene_manager_->get_current_scene();
    auto scene_items = scene->get_all_scene_items();

    const int total = static_cast<int>(scene_items.size());
    for (int i = total - 1; i >= 0; --i) {
        auto item = scene_items[i];

        auto* lw_item = new QListWidgetItem(listWidget_sceneItems_);
        lw_item->setSizeHint(QSize(240, 34));
        lw_item->setData(Qt::UserRole, QString::fromStdString(item->get_source_id()));

        QString display_name = extract_source_name(item->get_source());
        auto* row = new SceneItemRow(item, display_name, listWidget_sceneItems_);

        const int row_index = listWidget_sceneItems_->row(lw_item);
        row->set_move_up_enabled(row_index > 0);

        connect(row, &SceneItemRow::visibilityToggled, this, [this]() {
            sync_scene_to_compositor();
            if (canvas_widget_) canvas_widget_->refresh();
        });

        connect(row, &SceneItemRow::requestSetting, this, [this](std::shared_ptr<SceneItem> it) {
            if (!scene_manager_ || !scene_manager_->get_current_scene()) return;
            auto items = scene_manager_->get_current_scene()->get_all_scene_items();
            for (int idx = 0; idx < static_cast<int>(items.size()); ++idx) {
                if (items[idx] == it) {
                    show_scene_item_settings(idx);
                    return;
                }
            }
        });

        connect(row, &SceneItemRow::requestDelete, this, [this](std::shared_ptr<SceneItem> it) {
            if (!scene_manager_ || !scene_manager_->get_current_scene()) return;
            auto items = scene_manager_->get_current_scene()->get_all_scene_items();
            for (int idx = 0; idx < static_cast<int>(items.size()); ++idx) {
                if (items[idx] == it) {
                    delete_scene_item(idx);
                    return;
                }
            }
        });

        connect(row, &SceneItemRow::requestMoveUp, this, [this, lw_item](std::shared_ptr<SceneItem>) {
            if (!listWidget_sceneItems_) return;
            int r = listWidget_sceneItems_->row(lw_item);
            if (r <= 0) return;
            listWidget_sceneItems_->model()->moveRow(QModelIndex(), r, QModelIndex(), r - 1);
        });

        listWidget_sceneItems_->setItemWidget(lw_item, row);
    }
}

void MainWindow::on_scene_item_reordered() {
    if (!scene_manager_ || !scene_manager_->get_current_scene() || !listWidget_sceneItems_) return;

    auto scene = scene_manager_->get_current_scene();

    int order = listWidget_sceneItems_->count() - 1;
    for (int row = 0; row < listWidget_sceneItems_->count(); ++row, --order) {
        auto* lw_item = listWidget_sceneItems_->item(row);
        const QString sid = lw_item->data(Qt::UserRole).toString();
        if (sid.isEmpty()) continue;

        auto items = scene->get_all_scene_items();
        for (auto& it : items) {
            if (QString::fromStdString(it->get_source_id()) == sid) {
                it->set_order(order);
                break;
            }
        }
    }

    scene->normalize_orders();
    build_scene_list();
    sync_scene_to_compositor();
    if (canvas_widget_) canvas_widget_->refresh();
}

void MainWindow::set_live_id(const QString& live_id) {
    live_id_ = live_id;
    LOG_INFO("Live ID set: " + live_id_.toStdString());

    initialize_modules();
    setup_canvas_widget();
}

void MainWindow::set_rtmp_target(const QString& server_url, const QString& stream_key) {
    rtmp_server_url_ = server_url;
    rtmp_stream_key_ = stream_key;
}

void MainWindow::initialize_modules() {
    scene_manager_ = std::make_shared<SceneManager>();
    video_engine_ = std::make_shared<VideoEngine>();
    audio_engine_ = std::make_shared<AudioEngine>();
    encoder_ = std::make_shared<Encoder>();
    stream_pusher_ = std::make_shared<StreamPusher>();

    compositor_ = std::make_shared<Compositor>();
    encoder_bridge_ = std::make_shared<CompositorEncoderBridge>();

    capture_manager_ = std::make_shared<CaptureManagerIface>();

    video_engine_->initialize(1920, 1080, 30);
    audio_engine_->initialize(44100, 2);
    // Default: start microphone capture when entering live room
    if (audio_engine_) {
        LOG_INFO("Starting audio capture by default for live room");
        update_audio_status("🎤 初始化中...","orange");

        // Try to start audio capture
        bool audio_started = false;
        int retry_count = 3;

        for (int i = 0; i < retry_count && !audio_started; ++i) {
            if (i > 0) {
                LOG_INFO("Retrying audio capture startup (attempt " + std::to_string(i + 1) + ")");
                QThread::msleep(500); // Wait a bit before retry
            }

            if (audio_engine_->start_capture()) {
                audio_started = true;
                break;
            }
        }

        if (!audio_started) {
            LOG_WARNING("AudioEngine::start_capture failed after " + std::to_string(retry_count) + " attempts");
            update_audio_status("🎤 故障", "red");
            // enable silent audio fallback in encoder bridge
            if (encoder_bridge_) {
                encoder_bridge_->set_audio_engine(audio_engine_);
                encoder_bridge_->set_silent_audio(true);
            }
            QMessageBox::warning(this, "麦克风故障",
                "无法打开麦克风，程序将以静音推流作为回退。\n\n可能的原因：\n• 麦克风被其他程序占用\n• 音频设备驱动问题\n• 系统音频服务未运行\n\n请尝试：\n1. 检查麦克风是否被其他程序使用\n2. 重新启动应用程序\n3. 检查音频设备设置");
        } else {
            update_audio_status("🎤 正常", "green");
            if (encoder_bridge_) {
                encoder_bridge_->set_audio_engine(audio_engine_);
                encoder_bridge_->set_silent_audio(false);
            }
        }
    }

    video_engine_->set_current_scene(scene_manager_->get_current_scene());

    VideoEncoderConfig video_config;
    video_config.width = 1920;
    video_config.height = 1080;
    video_config.fps = 30;
    video_config.bitrate = 2500000;
    video_config.gop = 60; // Reduce GOP size for faster keyframe interval (2 seconds at 30fps)
    video_config.b_frames_enabled = false;
    encoder_->initialize_video_encoder(video_config);

    AudioEncoderConfig audio_config;
    audio_config.sample_rate = 44100;
    audio_config.channels = 2;
    audio_config.bitrate = 128000;
    encoder_->initialize_audio_encoder(audio_config);

    encoding_timer_ = new QTimer(this);
    connect(encoding_timer_, &QTimer::timeout, this, &MainWindow::encode_and_push);
    encoding_timer_->start(33);

    streams_registered_ = false;
    // Live duration label (added to status bar)
    live_duration_label_ = new QLabel("00:00:00", this);
    live_duration_label_->setMinimumWidth(100);

    // Audio status indicator
    audio_status_label_ = new QLabel("🎤 未初始化", this);
    audio_status_label_->setMinimumWidth(120);
    audio_status_label_->setStyleSheet("color: orange; font-weight: bold;");

    if (ui->statusBar) {
        // Ensure status bar is visible
        ui->statusBar->setVisible(true);
        ui->statusBar->addWidget(audio_status_label_);
        ui->statusBar->addPermanentWidget(live_duration_label_);

        // Force status bar update
        ui->statusBar->update();
    }
    live_duration_timer_ = new QTimer(this);
    connect(live_duration_timer_, &QTimer::timeout, this, [this]() {
        if (streaming_start_time_ms_ == 0) return;
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        qint64 elapsed_ms = now - streaming_start_time_ms_;
        qint64 s = elapsed_ms / 1000;
        qint64 hh = s / 3600;
        qint64 mm = (s % 3600) / 60;
        qint64 ss = s % 60;
        QString text = QString("%1:%2:%3")
            .arg(hh, 2, 10, QChar('0'))
            .arg(mm, 2, 10, QChar('0'))
            .arg(ss, 2, 10, QChar('0'));
        if (live_duration_label_) live_duration_label_->setText(text);
    });
}

void MainWindow::setup_ui_connections() {
    // Settings dialog open (both gears)
    auto open_settings_dialog = [this]() {
        if (!encoder_ || !audio_engine_) {
            QMessageBox::warning(this, "错误", "编码器或音频模块未初始化");
            return;
        }

        SettingsDialog dlg(this);
        dlg.set_current_video_config(encoder_->get_video_config());
        dlg.set_current_audio_config(encoder_->get_audio_config());
        dlg.set_available_microphones(audio_engine_->get_available_microphones(), audio_engine_->get_selected_microphone_id());

        if (dlg.exec() == QDialog::Accepted) {
            // Apply
            auto new_v = dlg.get_video_config();
            auto new_a = dlg.get_audio_config();
            const std::string mic_id = dlg.get_selected_microphone_id();

            // Reinit audio engine/encoder
            audio_engine_->initialize(new_a.sample_rate, new_a.channels);
            encoder_->reinitialize_audio_encoder(new_a);

            // Reinit video encoder + bridge settings
            encoder_->reinitialize_video_encoder(new_v);
            if (encoder_bridge_) {
                encoder_bridge_->set_resolution(new_v.width, new_v.height);
                encoder_bridge_->set_fps(new_v.fps);
            }

            // Select microphone (phase1: mic only)
            if (!mic_id.empty()) {
                audio_engine_->select_microphone(mic_id);
            }

            // If pushing, require restart to keep header/codecpar consistent
            if (stream_pusher_ && stream_pusher_->is_pushing()) {
                QMessageBox::information(this, "提示", "参数已修改，将重启推流使其生效");
                if (encoder_bridge_) {
                    encoder_bridge_->stop();
                }
                stream_pusher_->stop();
                streams_registered_ = false;
            }
        }
    };

    if (ui->pushButton_settings_topBar) {
        connect(ui->pushButton_settings_topBar, &QPushButton::clicked, this, open_settings_dialog);
    }

    if (ui->pushButton_settings) {
        connect(ui->pushButton_settings, &QPushButton::clicked, this, open_settings_dialog);
    }

    // 状态标签初始化
    if (ui->label_status) {
        ui->label_status->setText("预览中");
        ui->label_status->setStyleSheet("font-weight: bold; font-size: 14px; color: #00aa00;");
    }

    if (ui->pushButton_addMaterial) {
        connect(ui->pushButton_addMaterial, &QPushButton::clicked, this, [this]() {
            LOG_INFO("Add material dialog opened");
            AddMaterialDialog dlg(this);
            if (dlg.exec() == QDialog::Accepted) {
                switch (dlg.selected()) {
                    case AddMaterialDialog::Selection::Camera:
                        show_camera_selector();
                        break;
                    case AddMaterialDialog::Selection::Screen:
                        on_screen_share_button_clicked();
                        break;
                    default:
                        QMessageBox::information(this, "提示", "功能开发中...");
                        break;
                }
            }
        });
    }

    if (ui->pushButton_startLive) {
        connect(ui->pushButton_startLive, &QPushButton::clicked, this, [this]() {
            if (!encoder_bridge_) {
                QMessageBox::warning(this, "错误", "推流系统未初始化");
                return;
            }

            if (encoder_bridge_->is_streaming()) {
                // 停止推流
                encoder_bridge_->stop_streaming();
                ui->pushButton_startLive->setText("开始直播");
                if (ui->label_status) {
                    ui->label_status->setText("推流结束");
                    ui->label_status->setStyleSheet("font-weight: bold; font-size: 14px; color: #aa0000;");
                }
                QMessageBox::information(this, "提示", "推流已停止");
                return;
            }

            LOG_INFO("[DIAG] 准备开始推流");
            // 开始推流
            bool ok;
            QString url = QInputDialog::getText(this, "推流地址",
                                      "请输入RTMP推流地址:",
                                      QLineEdit::Normal,
                                      "rtmp://47.92.156.37:1935/live/aaa", &ok);
            if (!ok || url.isEmpty()) {
                return;
            }

            if (encoder_bridge_->start_streaming(url.toStdString())) {
                ui->pushButton_startLive->setText("停止直播");
                if (ui->label_status) {
                    ui->label_status->setText("正在推流");
                    ui->label_status->setStyleSheet("font-weight: bold; font-size: 14px; color: #00aa00;");
                }
                QMessageBox::information(this, "成功", "推流已启动");
            } else {
                QMessageBox::warning(this, "错误", "开始推流失败，请检查推流地址");
            }
        });
    }

    // Audio control connections
    if (ui->pushButton_mic) {
        connect(ui->pushButton_mic, &QPushButton::clicked, this, [this]() {
            toggle_microphone();
        });

        // Right-click context menu for microphone selection
        ui->pushButton_mic->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(ui->pushButton_mic, &QPushButton::customContextMenuRequested, this, [this](const QPoint& pos) {
            show_microphone_menu(ui->pushButton_mic->mapToGlobal(pos));
        });
    }

    if (ui->slider_mic) {
        connect(ui->slider_mic, &QSlider::valueChanged, this, [this](int value) {
            set_microphone_volume(value / 100.0f);
        });
    }

    if (ui->pushButton_speaker) {
        connect(ui->pushButton_speaker, &QPushButton::clicked, this, [this]() {
            toggle_speaker();
        });

        // Right-click context menu for speaker selection (placeholder for future)
        ui->pushButton_speaker->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(ui->pushButton_speaker, &QPushButton::customContextMenuRequested, this, [this](const QPoint& pos) {
            show_speaker_menu(ui->pushButton_speaker->mapToGlobal(pos));
        });
    }

    if (ui->slider_speaker) {
        connect(ui->slider_speaker, &QSlider::valueChanged, this, [this](int value) {
            set_speaker_volume(value / 100.0f);
        });
    }

    // Initialize audio controls
    update_microphone_ui();
    update_speaker_ui();
}

void MainWindow::on_camera_button_clicked() {
    LOG_INFO("Camera button clicked");
    if (is_camera_preview_) {
        stop_camera_preview();
    } else {
        show_camera_selector();
    }
}

void MainWindow::show_camera_selector() {
    if (!video_engine_) {
        LOG_ERROR("Video engine not initialized");
        QMessageBox::warning(this, "错误", "视频引擎未初始化");
        return;
    }

    LOG_INFO("开始获取可用摄像头列表");
    auto camera_choices = video_engine_->get_available_camera_choices();
    LOG_INFO("获取到 " + std::to_string(camera_choices.size()) + " 个摄像头设备");
    
    if (camera_choices.empty()) {
        LOG_WARNING("No cameras found");
        QMessageBox::information(this, "提示", "未检测到摄像头设备");
        return;
    }

    // 打印每个摄像头的详细信息
    for (const auto& camera : camera_choices) {
        LOG_INFO("摄像头设备: " + camera.display_name + " (DShow名称: " + camera.dshow_name + ")");
    }

    CameraSettingsDialog dialog(this);
    dialog.set_available_camera_choices(camera_choices);
    dialog.set_resolution("640x360");
    dialog.set_fps(30);
    dialog.set_pixel_format("PIXEL_FORMAT_YUY2");

    if (dialog.exec() == QDialog::Accepted) {
        const std::string camera_device_id = dialog.get_camera_device_id();
        QString selected_camera = QString::fromStdString(dialog.get_camera_name());

        std::string resolution = dialog.get_resolution();
        int fps = dialog.get_fps();
        std::string pixel_format = dialog.get_pixel_format();

        LOG_INFO("选中摄像头: " + selected_camera.toStdString() + " (ID: " + camera_device_id + ")");
        LOG_INFO("摄像头参数 - 分辨率: " + resolution + ", 帧率: " + std::to_string(fps) + ", 像素格式: " + pixel_format);

        if (video_engine_) {
            video_engine_->set_camera_resolution(resolution);
            video_engine_->set_camera_fps(fps);
            video_engine_->set_camera_pixel_format(pixel_format);
        }

        if (camera_device_id.empty()) {
            LOG_ERROR("摄像头设备标识无效");
            QMessageBox::warning(this, "错误", "摄像头设备标识无效");
            return;
        }
        on_select_camera(selected_camera, camera_device_id);
    }
}

void MainWindow::on_select_camera(const QString& camera_name, const std::string& camera_device_id) {
    LOG_INFO("Selected camera: '" + camera_name.toStdString() + "' with device_id: " + camera_device_id);

    if (!capture_manager_) {
        LOG_ERROR("采集管理器未初始化");
        QMessageBox::warning(this, "错误", "采集管理器未初始化");
        return;
    }

    // Use a hash of the unique device_id as the source_id to ensure stability and prevent illegal characters.
    const std::string source_id = "camera_" + std::to_string(std::hash<std::string>{}(camera_device_id));
    LOG_INFO("生成的源ID: " + source_id);

    // Dedup: do not allow opening the same physical device twice
    if (capture_manager_->has_source(source_id)) {
        LOG_INFO("该摄像头已在使用中: " + source_id);
        QMessageBox::information(this, "提示", "该摄像头已在使用中");
        return;
    }

    LOG_INFO("创建摄像头采集配置");
    CaptureConfig cfg;
    cfg.type = CaptureConfig::TargetType::CAMERA;
    cfg.target_id = camera_device_id; // Use dshow device_name
    cfg.fps = 30;
    LOG_INFO("采集配置: 类型=CAMERA, 目标ID=" + cfg.target_id + ", 帧率=" + std::to_string(cfg.fps));

    LOG_INFO("调用CaptureFactory::create_capture_source创建采集源");
    auto src = CaptureFactory::create_capture_source(cfg);
    if (!src) {
        LOG_ERROR("创建摄像头采集源失败");
        QMessageBox::warning(this, "错误", "创建摄像头采集源失败");
        return;
    }
    LOG_INFO("成功创建摄像头采集源");

    LOG_INFO("连接frameReady信号到Compositor的槽函数");
    // 使用信号槽连接替代回调，显式指定跨线程连接类型
    connect(src.get(), &ICaptureSource::frameReady, this, [this, source_id](const CaptureFrame& frame) {
        LOG_INFO("[DIAG] 收到frameReady信号，源ID: " + source_id + ", 图像尺寸: " + std::to_string(frame.image.width()) + "x" + std::to_string(frame.image.height()));

        // 更新compositor（用于推流）
        if (compositor_ && !frame.image.isNull()) {
            if (!compositor_->has_layer(source_id)) {
                LOG_INFO("[DIAG] 图层不存在，创建新图层: " + source_id);
                compositor_->add_layer(source_id);
            }
            LOG_INFO("[DIAG] 更新Compositor图层图像: " + source_id);
            compositor_->updateLayerImage(QString::fromStdString(source_id), frame.image);
        }

        // 更新对应的ScreenSource或CameraSource（用于预览显示）
        if (scene_manager_ && scene_manager_->get_current_scene()) {
            auto scene = scene_manager_->get_current_scene();
            auto items = scene->get_all_scene_items();
            for (auto& item : items) {
                if (item && item->get_source_id() == source_id) {
                    // 尝试更新 ScreenSource（屏幕共享）
                    auto screenSrc = std::dynamic_pointer_cast<ScreenSource>(item->get_source());
                    if (screenSrc) {
                        LOG_INFO("[DIAG] 更新ScreenSource图像: " + source_id);
                        screenSrc->push_frame(frame.image);
                        break;
                    }

                    // 尝试更新 CameraSource（摄像头）
                    auto cameraSrc = std::dynamic_pointer_cast<CameraSource>(item->get_source());
                    if (cameraSrc) {
                        LOG_INFO("[DIAG] 更新CameraSource图像: " + source_id);
                        cameraSrc->push_frame(frame.image);
                        break;
                    }
                }
            }
        }
    }, Qt::QueuedConnection);
    LOG_INFO("信号槽连接成功");

    LOG_INFO("将采集源添加到采集管理器: " + source_id);
    if (!capture_manager_->add_source(source_id, src)) {
        LOG_ERROR("添加摄像头采集源失败（可能初始化失败）");
        QMessageBox::warning(this, "错误", "添加摄像头采集源失败（可能初始化失败）");
        return;
    }
    LOG_INFO("采集源添加成功");

    if (scene_manager_ && scene_manager_->get_current_scene()) {
        LOG_INFO("将摄像头源添加到场景中");
        auto scene = scene_manager_->get_current_scene();
        auto camera_source = SourceFactory::create_camera_source(source_id, camera_name.toStdString());
        LOG_INFO("初始化场景摄像头源");
        camera_source->initialize();
        LOG_INFO("启动场景摄像头源");
        camera_source->start();
        LOG_INFO("添加到场景");
        scene->add_source(camera_source);
        LOG_INFO("更新场景项并同步到Compositor");
        update_scene_items();  // 先调用，确保compositor中有对应的图层
        LOG_INFO("Added camera source to scene: " + camera_name.toStdString() + ", id=" + source_id);
    }

    // 最后启动摄像头源，确保此时compositor中已经有了对应的图层
    LOG_INFO("启动摄像头采集源: " + source_id);
    if (!capture_manager_->start_source(source_id)) {
        LOG_ERROR("启动摄像头采集源失败");
        capture_manager_->remove_source(source_id);
        QMessageBox::warning(this, "错误", "启动摄像头采集源失败");
        return;
    }
    LOG_INFO("摄像头采集源启动成功: " + source_id);
}

void MainWindow::start_camera_preview() {
    if (!video_engine_) {
        LOG_ERROR("Video engine not initialized");
        return;
    }

    bool result = video_engine_->start_capture();
    if (!result) {
        LOG_ERROR("Failed to start camera capture");
        QMessageBox::warning(this, "错误", "启动摄像头失败");
        return;
    }

    is_camera_preview_ = true;
    preview_timer_->start(33);
    LOG_INFO("Camera preview started");
}

void MainWindow::stop_camera_preview() {
    if (!video_engine_) {
        return;
    }

    video_engine_->stop_capture();
    preview_timer_->stop();
    is_camera_preview_ = false;
    LOG_INFO("Camera preview stopped");
}

void MainWindow::on_screen_share_button_clicked() {
    LOG_INFO("Screen share button clicked");
    show_screen_share_selector();
}

void MainWindow::show_screen_share_selector() {
    LOG_INFO("Showing screen capture selector");
    ScreenCaptureSelector dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        const auto* selected_target = dialog.get_selected_target();
        if (selected_target) {
            LOG_INFO(std::string("选择了共享目标: ID=") + selected_target->id + ", 类型=" + (selected_target->type == CaptureTarget::Type::SCREEN ? "屏幕" : "窗口"));
            int fps = 30;
            QString resolution = "原尺寸";
            bool capture_cursor = true;
            bool capture_border = (selected_target->type == CaptureTarget::Type::WINDOW);

            LOG_INFO(std::string("创建共享屏幕采集配置"));
            CaptureConfig cfg;
            cfg.type = selected_target->type == CaptureTarget::Type::SCREEN ? CaptureConfig::TargetType::SCREEN : CaptureConfig::TargetType::WINDOW;
            cfg.target_id = selected_target->id;
            cfg.fps = fps;
            cfg.capture_cursor = capture_cursor;
            cfg.capture_border = capture_border;
            LOG_INFO(std::string("采集配置: 类型=") + (cfg.type == CaptureConfig::TargetType::SCREEN ? "SCREEN" : "WINDOW") + ", 目标ID=" + cfg.target_id + ", 帧率=" + std::to_string(cfg.fps) + ", 捕获鼠标=" + (cfg.capture_cursor ? "是" : "否") + ", 捕获边框=" + (cfg.capture_border ? "是" : "否"));

            LOG_INFO(std::string("调用CaptureFactory::create_capture_source创建采集源"));
            auto src = CaptureFactory::create_capture_source(cfg);
            if (src) {
                std::string source_id = std::string("capture_") + selected_target->id;
                LOG_INFO(std::string("生成的源ID: ") + source_id);

                LOG_INFO(std::string("连接frameReady信号到Compositor的槽函数"));
                // 使用信号槽连接替代回调，显式指定跨线程连接类型
                connect(src.get(), &ICaptureSource::frameReady, this, [this, source_id](const CaptureFrame& frame) {
                    LOG_INFO("   收到frameReady信号，源ID: " + source_id + ", 图像尺寸: " + std::to_string(frame.image.width()) + "x" + std::to_string(frame.image.height()));

                    // 对于屏幕共享，同时更新compositor和ScreenSource
                    if (source_id.find("capture_") == 0) {  // 屏幕共享源ID以"capture_"开头
                        // 更新compositor（用于推流）
                        if (compositor_ && !frame.image.isNull()) {
                            if (!compositor_->has_layer(source_id)) {
                                LOG_INFO("[DIAG] 图层不存在，创建新图层: " + source_id);
                                compositor_->add_layer(source_id);
                            }
                            LOG_INFO("[DIAG] 更新Compositor图层图像: " + source_id);
                            compositor_->updateLayerImage(QString::fromStdString(source_id), frame.image);
                        }

                        // 更新对应的ScreenSource或CameraSource（用于预览显示）
                        if (scene_manager_ && scene_manager_->get_current_scene()) {
                            auto scene = scene_manager_->get_current_scene();
                            auto items = scene->get_all_scene_items();
                            for (auto& item : items) {
                                if (item && item->get_source_id() == source_id) {
                                    // 尝试更新 ScreenSource（屏幕共享）
                                    auto screenSrc = std::dynamic_pointer_cast<ScreenSource>(item->get_source());
                                    if (screenSrc) {
                                        LOG_INFO("[DIAG] 更新ScreenSource图像: " + source_id);
                                        screenSrc->push_frame(frame.image);
                                        break;
                                    }

                                    // 尝试更新 CameraSource（摄像头）
                                    auto cameraSrc = std::dynamic_pointer_cast<CameraSource>(item->get_source());
                                    if (cameraSrc) {
                                        LOG_INFO("[DIAG] 更新CameraSource图像: " + source_id);
                                        cameraSrc->push_frame(frame.image);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    // 对于摄像头，只更新ScreenSource（已在其他地方处理）
                }, Qt::QueuedConnection);
                LOG_INFO(std::string("信号槽连接成功"));

                LOG_INFO(std::string("将采集源添加到采集管理器: ") + source_id);
                if (capture_manager_->add_source(source_id, src)) {
                    LOG_INFO(std::string("启动采集源: ") + source_id);
                    capture_manager_->start_source(source_id);
                    LOG_INFO(std::string("Started capture source: ") + source_id);
                } else {
                    LOG_ERROR(std::string("Failed to add capture source: ") + source_id);
                }

                if (scene_manager_ && scene_manager_->get_current_scene()) {
                    LOG_INFO(std::string("将屏幕共享源添加到场景中"));
                    auto scene = scene_manager_->get_current_scene();
                    auto screen_src = SourceFactory::create_screen_source(source_id, selected_target->name);
                    if (screen_src) {
                        screen_src->initialize();
                        screen_src->start();
                        scene->add_source(screen_src);
                        update_scene_items();
                        LOG_INFO(std::string("Added ScreenSource to scene for preview: ") + source_id);
                    }
                }
            } else {
                LOG_ERROR(std::string("创建屏幕共享采集源失败"));
            }

            on_select_screen_share(
                QString::fromStdString(selected_target->id),
                selected_target->type == CaptureTarget::Type::SCREEN,
                fps,
                resolution,
                capture_cursor,
                capture_border);
        }
    }
}

void MainWindow::on_select_screen_share(const QString& target_id, bool is_screen_mode, int fps, const QString& resolution, bool capture_cursor, bool capture_border) {
    LOG_INFO("Selected screen share target: " + target_id.toStdString());

    QString target_type = is_screen_mode ? "屏幕" : "窗口";
    std::string source_id = std::string("capture_") + target_id.toStdString();

    if (capture_manager_ && capture_manager_->has_source(source_id)) {
        QMessageBox::information(this, "屏幕共享",
            QString("已开始共享 %1 (ID: %2)\n"
                    "帧率: %3 fps\n"
                    "分辨率: %4\n"
                    "捕获鼠标: %5\n"
                    "捕获边框: %6")
            .arg(target_type)
            .arg(target_id)
            .arg(fps)
            .arg(resolution)
            .arg(capture_cursor ? "是" : "否")
            .arg(capture_border ? "是" : "否"));
    } else {
        QMessageBox::information(this, "屏幕共享",
            QString("已选择 %1 目标 (ID: %2)\n"
                    "帧率: %3 fps\n"
                    "分辨率: %4\n"
                    "捕获鼠标: %5\n"
                    "捕获边框: %6\n\n"
                    "WGC 捕获功能正在开发中（系统将回退到兼容方案以继续预览）。")
            .arg(target_type)
            .arg(target_id)
            .arg(fps)
            .arg(resolution)
            .arg(capture_cursor ? "是" : "否")
            .arg(capture_border ? "是" : "否"));
    }
}

void MainWindow::on_camera_frame_ready() {
}

void MainWindow::update_status(const QString& message) {
    ui->statusBar->showMessage(message);
}

void MainWindow::setup_canvas_widget() {
    canvas_widget_ = new CanvasWidget(this);
    canvas_widget_->set_scene_manager(scene_manager_);
    canvas_widget_->set_video_engine(video_engine_);
    // 使用默认的画布配置（横屏16:9）
    set_canvas_config(canvas_config_);

    canvas_widget_->set_compositor(compositor_);

    encoder_bridge_->set_compositor(compositor_);
    encoder_bridge_->set_encoder(encoder_);
    encoder_bridge_->set_stream_pusher(stream_pusher_);
    encoder_bridge_->set_audio_engine(audio_engine_);
    encoder_bridge_->set_resolution(1920, 1080);
    // Start the encoder bridge so it begins capturing/compositing frames for streaming.
    if (encoder_ && encoder_bridge_) {
        encoder_bridge_->set_fps(static_cast<int>(encoder_->get_video_config().fps));
        encoder_bridge_->start(static_cast<int>(encoder_->get_video_config().fps));
        LOG_INFO("Encoder bridge started from MainWindow with fps: " + std::to_string(encoder_->get_video_config().fps));
    }

    // 连接推流控制信号
    connect(encoder_bridge_.get(), &CompositorEncoderBridge::streaming_started,
            this, &MainWindow::on_streaming_started);
    connect(encoder_bridge_.get(), &CompositorEncoderBridge::streaming_stopped,
            this, &MainWindow::on_streaming_stopped);
    connect(encoder_bridge_.get(), &CompositorEncoderBridge::streaming_error,
            this, &MainWindow::on_streaming_error);

    ui->verticalLayout_liveArea->removeWidget(ui->label_livePreview);
    delete ui->label_livePreview;
    ui->label_livePreview = nullptr;

    ui->verticalLayout_liveArea->insertWidget(0, canvas_widget_);

    connect(canvas_widget_, &CanvasWidget::scene_item_selected, this, [this](std::shared_ptr<SceneItem> item) {
        LOG_INFO("Scene item selected: " + (item ? item->get_source_id() : "null"));
    });

    connect(canvas_widget_, &CanvasWidget::scene_item_moved, this,
        [this](std::shared_ptr<SceneItem>, const Transform&, const Transform&) {
            sync_scene_to_compositor();
        });

    LOG_INFO("Canvas widget setup completed");
}

void MainWindow::update_preview() {
    if (canvas_widget_) {
        canvas_widget_->refresh();
    }

    // Camera frames are now driven by CaptureManager camera sources.
}

void MainWindow::encode_and_push() {
    if (!stream_pusher_ || !stream_pusher_->is_pushing()) {
        return;
    }

    // Video: pushed by CompositorEncoderBridge (scene output). Keep audio only here.

    // Audio - Always generate audio frames to keep stream alive
    auto audio_frame = audio_engine_->get_audio_frame();
    if (!audio_frame) {
        // Generate silent audio frame when no audio is available
        int sample_rate = audio_engine_->get_sample_rate() > 0 ? audio_engine_->get_sample_rate() : 44100;
        int channels = audio_engine_->get_channels() > 0 ? audio_engine_->get_channels() : 2;
        int samples_per_frame = 1024; // Standard AAC frame size

        audio_frame = std::make_shared<AudioFrame>(sample_rate, channels, samples_per_frame);
        LOG_DEBUG("[MAIN] Generated silent audio frame: " + std::to_string(sample_rate) + "Hz, " +
                 std::to_string(channels) + "ch, " + std::to_string(samples_per_frame) + " samples");
    } else {
        LOG_DEBUG("[MAIN] Got real audio frame: sample_rate=" + std::to_string(audio_frame->sample_rate) +
                 ", channels=" + std::to_string(audio_frame->channels) + ", samples=" + std::to_string(audio_frame->samples));
    }

    // Set timestamps for audio frame (use elapsed time from streaming start)
    auto current_timestamp_us = media_clock_.get_elapsed_time_us();
    audio_frame->timestamp = MediaTimestamp(current_timestamp_us);
    audio_frame->timestamp_ms = current_timestamp_us / 1000;

    std::vector<EncodedPacketPtr> packets;
    ErrorCode result = encoder_->encode_audio_frame(audio_frame, packets);
    LOG_INFO("[MAIN] audio encode result=" + std::to_string(static_cast<int>(result)) +
             ", packets=" + std::to_string(packets.size()));

    if (result == ErrorCode::SUCCESS && !packets.empty()) {
        for (auto& p : packets) {
            if (!p) continue;
            p->wallclock_us = current_timestamp_us;
            ErrorCode push_ret = stream_pusher_->push_packet(p);
            LOG_INFO("[MAIN] push_packet returned: " + std::to_string(static_cast<int>(push_ret)));
        }
    }
}

void MainWindow::update_scene_items() {
    build_scene_list();
    sync_scene_to_compositor();
}

void MainWindow::toggle_scene_item_visibility(int index) {
    if (!scene_manager_ || !scene_manager_->get_current_scene()) {
        return;
    }

    auto scene = scene_manager_->get_current_scene();
    auto items = scene->get_all_scene_items();
    if (index < 0 || index >= static_cast<int>(items.size())) {
        return;
    }

    items[index]->set_visible(!items[index]->is_visible());
    update_scene_items();
    if (canvas_widget_) canvas_widget_->refresh();
}

void MainWindow::show_scene_item_settings(int /*index*/) {
    QMessageBox::information(this, "提示", "设置功能开发中...");
}

void MainWindow::delete_scene_item(int index) {
    if (!scene_manager_ || !scene_manager_->get_current_scene()) {
        return;
    }

    auto scene = scene_manager_->get_current_scene();
    auto items = scene->get_all_scene_items();
    if (index < 0 || index >= static_cast<int>(items.size())) {
        return;
    }

    auto item = items[index];
    std::string sid = item->get_source_id();

    // clear ScreenSource frame if needed
    if (auto source = item->get_source()) {
        auto screenSrc = std::dynamic_pointer_cast<ScreenSource>(source);
        if (screenSrc) {
            screenSrc->push_frame(QImage());
        }
    }

    scene->remove_scene_item(item);

    if (capture_manager_ && capture_manager_->has_source(sid)) {
        capture_manager_->stop_source(sid);
        capture_manager_->remove_source(sid);
    }

    if (compositor_ && compositor_->has_layer(sid)) {
        compositor_->remove_layer(sid);
    }

    if (sid.rfind("camera_", 0) == 0) {
        if (is_camera_preview_) stop_camera_preview();
    }

    update_scene_items();
    if (canvas_widget_) canvas_widget_->refresh();
}

QString MainWindow::extract_source_name(std::shared_ptr<Source> source) {
    if (!source) return "Unknown";

    const std::string metadata = source->get_metadata();
    if (!metadata.empty()) {
        const std::string key = "name:";
        size_t pos = metadata.find(key);
        if (pos != std::string::npos) {
            pos += key.size();
            size_t end = metadata.find(',', pos);
            std::string n = (end == std::string::npos) ? metadata.substr(pos) : metadata.substr(pos, end - pos);
            if (!n.empty()) {
                return QString::fromStdString(n);
            }
        }
    }

    return QString::fromStdString(source->get_id());
}

QPushButton* MainWindow::create_scene_item_button(const QString& text, const QString& style) {
    QPushButton* button = new QPushButton(text);
    if (!style.isEmpty()) {
        button->setStyleSheet(style);
    }
    button->setMinimumHeight(30);
    button->setMaximumHeight(30);
    return button;
}

QPushButton* MainWindow::create_icon_button(const QString& icon_text, const QString& tooltip) {
    QPushButton* button = new QPushButton(icon_text);
    if (!tooltip.isEmpty()) {
        button->setToolTip(tooltip);
    }
    button->setMaximumSize(30, 30);
    button->setStyleSheet("QPushButton { border: none; background: transparent; font-size: 14px; }");
    return button;
}

QPushButton* MainWindow::create_icon_button(QStyle::StandardPixmap icon, const QString& tooltip) {
    QPushButton* button = new QPushButton();
    button->setIcon(style()->standardIcon(icon));
    button->setIconSize(QSize(16, 16));
    if (!tooltip.isEmpty()) {
        button->setToolTip(tooltip);
    }
    button->setMaximumSize(30, 30);
    button->setStyleSheet("QPushButton { border: none; background: transparent; }");
    return button;
}

void MainWindow::sync_scene_to_compositor() {
    if (!compositor_ || !scene_manager_ || !scene_manager_->get_current_scene()) {
        return;
    }

    auto scene = scene_manager_->get_current_scene();
    auto items = scene->get_all_scene_items();

    std::unordered_set<std::string> active;
    active.reserve(items.size());

    for (auto &it : items) {
        if (!it) continue;
        auto src = it->get_source();
        if (!src) continue;

        const std::string sid = src->get_id();
        active.insert(sid);

        if (!compositor_->has_layer(sid)) {
            compositor_->add_layer(sid);
        }

        const auto tr = it->get_transform();
        const int w = tr.width > 0 ? tr.width : 640;
        const int h = tr.height > 0 ? tr.height : 360;

        // 确保视频内容在画布范围内，不超出边界
        int canvas_width = canvas_widget_->get_canvas_config().get_width();
        int canvas_height = canvas_widget_->get_canvas_config().get_height();

        int clamped_x = (std::max)(0, (std::min)(tr.x, canvas_width - w));
        int clamped_y = (std::max)(0, (std::min)(tr.y, canvas_height - h));
        int clamped_w = (std::min)(w, canvas_width - clamped_x);
        int clamped_h = (std::min)(h, canvas_height - clamped_y);

        compositor_->update_layer_transform(sid, QRectF(clamped_x, clamped_y, clamped_w, clamped_h), tr.opacity);
        compositor_->set_layer_visible(sid, it->is_visible());
        compositor_->set_layer_order(sid, it->get_order());
    }

    for (const auto& lid : compositor_->get_layer_ids()) {
        if (active.find(lid) == active.end()) {
            compositor_->remove_layer(lid);
        }
    }
}

void MainWindow::update_audio_status(const QString& text, const QString& color) {
    if (audio_status_label_) {
        audio_status_label_->setText(text);
        if (!color.isEmpty()) {
            audio_status_label_->setStyleSheet("color: " + color + ";");
        }
    }
}

void MainWindow::toggle_microphone() {
    microphone_enabled_ = !microphone_enabled_;
    if (audio_engine_) {
        audio_engine_->set_microphone_mute(!microphone_enabled_);
    }
    update_microphone_ui();
    LOG_INFO(std::string("Microphone ") + (microphone_enabled_ ? "enabled" : "disabled"));
}

void MainWindow::set_microphone_volume(float volume) {
    if (audio_engine_) {
        audio_engine_->set_microphone_volume(volume);
    }
    if (ui->label_micLevel) {
        ui->label_micLevel->setText(QString::number(static_cast<int>(volume * 100)) + "%");
    }
    // Update slider position
    if (ui->slider_mic) {
        ui->slider_mic->setValue(static_cast<int>(volume * 100));
    }
}

void MainWindow::update_microphone_ui() {
    if (ui->pushButton_mic) {
        ui->pushButton_mic->setText(microphone_enabled_ ? "🎤" : "🎤❌");
        ui->pushButton_mic->setStyleSheet(microphone_enabled_ ?
            "border: none; background: transparent; font-size: 16px;" :
            "border: none; background: transparent; font-size: 16px; color: #ff6666;");
    }

    if (ui->slider_mic && audio_engine_) {
        int volume = static_cast<int>(audio_engine_->get_microphone_volume() * 100);
        ui->slider_mic->setValue(volume);
        if (ui->label_micLevel) {
            ui->label_micLevel->setText(QString::number(volume) + "%");
        }
    }
}

void MainWindow::show_microphone_menu(const QPoint& pos) {
    if (!audio_engine_) return;

    QMenu menu(this);
    menu.setTitle("选择麦克风");

    auto devices = audio_engine_->get_available_microphones();
    auto current_id = audio_engine_->get_selected_microphone_id();

    for (const auto& device : devices) {
        QAction* action = menu.addAction(QString::fromUtf8(device.name.c_str()));
        action->setCheckable(true);
        action->setChecked(device.id == current_id);

        connect(action, &QAction::triggered, this, [this, device_id = device.id]() {
            if (audio_engine_->select_microphone(device_id)) {
                LOG_INFO("Selected microphone: " + device_id);
                // Restart audio capture with new device
                audio_engine_->stop_capture();
                if (!audio_engine_->start_capture()) {
                    update_audio_status("🎤 故障", "red");
                } else {
                    update_audio_status("🎤 正常", "green");
                }
            }
        });
    }

    menu.exec(pos);
}

void MainWindow::toggle_speaker() {
    speaker_enabled_ = !speaker_enabled_;
    if (audio_engine_) {
        audio_engine_->set_speaker_mute(!speaker_enabled_);
    }
    update_speaker_ui();
    LOG_INFO(std::string("Speaker ") + (speaker_enabled_ ? "enabled" : "disabled"));
}

void MainWindow::set_speaker_volume(float volume) {
    if (audio_engine_) {
        audio_engine_->set_speaker_volume(volume);
    }
    if (ui->label_speakerLevel) {
        ui->label_speakerLevel->setText(QString::number(static_cast<int>(volume * 100)) + "%");
    }
    // Update slider position
    if (ui->slider_speaker) {
        ui->slider_speaker->setValue(static_cast<int>(volume * 100));
    }
}

void MainWindow::update_speaker_ui() {
    if (ui->pushButton_speaker) {
        ui->pushButton_speaker->setText(speaker_enabled_ ? "🔊" : "🔇");
        ui->pushButton_speaker->setStyleSheet(speaker_enabled_ ?
            "border: none; background: transparent; font-size: 16px;" :
            "border: none; background: transparent; font-size: 16px; color: #ff6666;");
    }

    if (ui->slider_speaker && audio_engine_) {
        int volume = static_cast<int>(audio_engine_->get_speaker_volume() * 100);
        ui->slider_speaker->setValue(volume);
        if (ui->label_speakerLevel) {
            ui->label_speakerLevel->setText(QString::number(volume) + "%");
        }
    }
}

void MainWindow::show_speaker_menu(const QPoint& pos) {
    // Placeholder for speaker device selection
    // In a full implementation, this would show available speaker devices
    QMenu menu(this);
    menu.setTitle("扬声器设备");

    QAction* placeholder = menu.addAction("默认扬声器 (功能开发中...)");
    placeholder->setEnabled(false);

    menu.exec(pos);
}

// 推流控制方法实现

void MainWindow::on_streaming_started() {
    LOG_INFO("推流状态：已开始");
    streaming_start_time_ms_ = QDateTime::currentMSecsSinceEpoch();
    if (live_duration_label_) live_duration_label_->setText("00:00:00");
    if (live_duration_timer_) live_duration_timer_->start(1000);
    // 可以在这里更新UI状态，比如显示"正在推流"的状态
}

void MainWindow::on_streaming_stopped() {
    LOG_INFO("推流状态：已停止");
    if (live_duration_timer_) live_duration_timer_->stop();
    streaming_start_time_ms_ = 0;
    if (live_duration_label_) live_duration_label_->setText("00:00:00");
    // 可以在这里更新UI状态
}

void MainWindow::on_streaming_error(const QString& error) {
    LOG_ERROR("推流错误: " + error.toStdString());
    QMessageBox::warning(this, "推流错误", error);
}

// 画布配置管理方法实现
void MainWindow::set_canvas_config(const CanvasConfig& config) {
    canvas_config_ = config;

    // 更新CanvasWidget
    if (canvas_widget_) {
        canvas_widget_->set_canvas_config(config);
    }

    // 更新编码器配置
    if (encoder_ && encoder_bridge_) {
        int width = config.get_width();
        int height = config.get_height();

        // 重新初始化视频编码器
        VideoEncoderConfig video_config;
        video_config.width = width;
        video_config.height = height;
        video_config.fps = 30;
        video_config.bitrate = 2500000;
        video_config.gop = 60;
        video_config.b_frames_enabled = false;

        encoder_->reinitialize_video_encoder(video_config);
        encoder_bridge_->set_resolution(width, height);
    }

    LOG_INFO("Canvas config updated to: " + config.get_name());
}

const CanvasConfig& MainWindow::get_canvas_config() const {
    return canvas_config_;
}

} // namespace live_assistant
