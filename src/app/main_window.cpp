#include "app/main_window.h"
#include "app/camera_settings.h"
#include "app/screen_share.h"
#include "app/screen_capture_selector.h"
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
#include <QInputDialog>
#include <QHBoxLayout>
#include <QStyle>
#include <QAbstractItemModel>
#include <QVariant>

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
    if (canvas_widget_) canvas_widget_->refresh();
}

void MainWindow::set_live_id(const QString& live_id) {
    live_id_ = live_id;
    LOG_INFO("Live ID set: " + live_id_.toStdString());

    initialize_modules();
    setup_canvas_widget();
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

    video_engine_->set_current_scene(scene_manager_->get_current_scene());

    VideoEncoderConfig video_config;
    video_config.width = 1920;
    video_config.height = 1080;
    video_config.fps = 30;
    video_config.bitrate = 2500000;
    video_config.gop = 30;
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
}

void MainWindow::setup_ui_connections() {
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
            LOG_INFO("Start live button clicked");

            if (!stream_pusher_ || !encoder_) {
                QMessageBox::warning(this, "错误", "推流器或编码器未初始化");
                return;
            }

            if (stream_pusher_->is_pushing()) {
                stream_pusher_->stop();
                streams_registered_ = false;
                QMessageBox::information(this, "提示", "已停止推流");
                return;
            }

            StreamConfig cfg;
            cfg.server_url = "rtmp://localhost/live";
            cfg.stream_key = "test";
            cfg.max_queue_size = 300;
            cfg.auto_reconnect = true;
            cfg.low_latency = true;

            ErrorCode cfg_ret = stream_pusher_->set_config(cfg);
            if (cfg_ret != ErrorCode::SUCCESS) {
                QMessageBox::warning(this, "错误", "设置推流配置失败");
                return;
            }

            if (!streams_registered_) {
                AVCodecParameters* a_par = encoder_->get_audio_codec_parameters();
                AVRational a_tb = encoder_->get_audio_time_base();
                AVCodecParameters* v_par = encoder_->get_video_codec_parameters();
                AVRational v_tb = encoder_->get_video_time_base();

                if (!a_par || a_tb.den <= 0 || !v_par || v_tb.den <= 0) {
                    if (a_par) avcodec_parameters_free(&a_par);
                    if (v_par) avcodec_parameters_free(&v_par);
                    QMessageBox::warning(this, "错误", "音频/视频编码器参数不可用（可能 H264 encoder 不存在）");
                    return;
                }

                ErrorCode ra = stream_pusher_->register_audio_stream(a_par, a_tb);
                ErrorCode rv = stream_pusher_->register_video_stream(v_par, v_tb);

                avcodec_parameters_free(&a_par);
                avcodec_parameters_free(&v_par);

                if (ra != ErrorCode::SUCCESS || rv != ErrorCode::SUCCESS) {
                    QMessageBox::warning(this, "错误", "注册音视频流失败");
                    return;
                }

                streams_registered_ = true;
            }

            ErrorCode start_ret = stream_pusher_->start();
            if (start_ret != ErrorCode::SUCCESS) {
                QMessageBox::warning(this, "错误", "启动推流失败");
                streams_registered_ = false;
                return;
            }

            QMessageBox::information(this, "提示", "已开始推流");
        });
    }
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

    std::vector<std::string> cameras = video_engine_->get_available_cameras();
    if (cameras.empty()) {
        LOG_WARNING("No cameras found");
        QMessageBox::information(this, "提示", "未检测到摄像头设备");
        return;
    }

    CameraSettingsDialog dialog(this);
    dialog.set_available_cameras(cameras);
    dialog.set_resolution("640x360");
    dialog.set_fps(30);
    dialog.set_pixel_format("PIXEL_FORMAT_YUY2");

    if (dialog.exec() == QDialog::Accepted) {
        QString selected_camera = QString::fromStdString(dialog.get_camera_name());

        std::string resolution = dialog.get_resolution();
        int fps = dialog.get_fps();
        std::string pixel_format = dialog.get_pixel_format();

        if (video_engine_) {
            video_engine_->set_camera_resolution(resolution);
            video_engine_->set_camera_fps(fps);
            video_engine_->set_camera_pixel_format(pixel_format);
        }

        on_select_camera(selected_camera);
    }
}

void MainWindow::on_select_camera(const QString& camera_name) {
    LOG_INFO("Selected camera: " + camera_name.toStdString());

    if (!video_engine_) {
        LOG_ERROR("Video engine not initialized");
        return;
    }

    bool result = video_engine_->select_camera(camera_name.toStdString());
    if (!result) {
        LOG_ERROR("Failed to select camera");
        QMessageBox::warning(this, "错误", "选择摄像头失败");
        return;
    }

    video_engine_->set_capture_mode(VideoEngine::CaptureMode::OPENCV);
    start_camera_preview();

    if (scene_manager_ && scene_manager_->get_current_scene()) {
        auto scene = scene_manager_->get_current_scene();

        std::string camera_id = "camera_" + std::to_string(rand());
        auto camera_source = SourceFactory::create_camera_source(camera_id, camera_name.toStdString());

        camera_source->initialize();
        camera_source->start();

        scene->add_source(camera_source);
        update_scene_items();

        LOG_INFO("Added camera source to scene: " + camera_name.toStdString());
    }
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
            int fps = 30;
            QString resolution = "原尺寸";
            bool capture_cursor = true;
            bool capture_border = (selected_target->type == CaptureTarget::Type::WINDOW);

            CaptureConfig cfg;
            cfg.type = selected_target->type == CaptureTarget::Type::SCREEN ? CaptureConfig::TargetType::SCREEN : CaptureConfig::TargetType::WINDOW;
            cfg.target_id = selected_target->id;
            cfg.fps = fps;
            cfg.capture_cursor = capture_cursor;
            cfg.capture_border = capture_border;

            auto src = CaptureFactory::create_capture_source(cfg);
            if (src) {
                std::string source_id = std::string("capture_") + selected_target->id;

                if (compositor_) {
                    compositor_->add_layer(source_id);
                    compositor_->update_layer_transform(source_id, QRectF(50, 50, 640, 360));
                    compositor_->set_layer_visible(source_id, true);
                }

                std::weak_ptr<Scene> weak_scene;
                if (scene_manager_ && scene_manager_->get_current_scene()) {
                    weak_scene = scene_manager_->get_current_scene();
                }

                src->set_frame_callback([this, source_id, weak_scene](const CaptureFrame& frame) {
                    LOG_INFO("Capture frame from " + source_id + ", size: " +
                             std::to_string(frame.width) + "x" + std::to_string(frame.height));

                    if (compositor_ && !frame.image.isNull()) {
                        compositor_->update_layer_image(source_id, frame.image);
                    }

                    if (!weak_scene.expired()) {
                        auto scene = weak_scene.lock();
                        if (scene) {
                            auto items = scene->get_all_scene_items();
                            for (auto &it : items) {
                                auto srcPtr = it->get_source();
                                if (srcPtr && srcPtr->get_id() == source_id) {
                                    auto screenSrc = std::dynamic_pointer_cast<ScreenSource>(srcPtr);
                                    if (screenSrc) {
                                        screenSrc->push_frame(frame.image);
                                    }
                                }
                            }
                        }
                    }
                });

                if (capture_manager_->add_source(source_id, src)) {
                    capture_manager_->start_source(source_id);
                    LOG_INFO("Started capture source: " + source_id);
                } else {
                    LOG_ERROR("Failed to add capture source: " + source_id);
                }

                if (scene_manager_ && scene_manager_->get_current_scene()) {
                    auto scene = scene_manager_->get_current_scene();
                    auto screen_src = SourceFactory::create_screen_source(source_id, selected_target->name);
                    if (screen_src) {
                        screen_src->initialize();
                        screen_src->start();
                        scene->add_source(screen_src);
                        update_scene_items();
                        LOG_INFO("Added ScreenSource to scene for preview: " + source_id);
                    }
                }
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
    canvas_widget_->set_canvas_resolution(1920, 1080);

    canvas_widget_->set_compositor(compositor_);

    encoder_bridge_->set_compositor(compositor_);
    encoder_bridge_->set_encoder(encoder_);
    encoder_bridge_->set_stream_pusher(stream_pusher_);
    encoder_bridge_->set_resolution(1920, 1080);

    ui->verticalLayout_liveArea->removeWidget(ui->label_livePreview);
    delete ui->label_livePreview;
    ui->label_livePreview = nullptr;

    ui->verticalLayout_liveArea->insertWidget(0, canvas_widget_);

    connect(canvas_widget_, &CanvasWidget::scene_item_selected, this, [this](std::shared_ptr<SceneItem> item) {
        LOG_INFO("Scene item selected: " + (item ? item->get_source_id() : "null"));
    });

    LOG_INFO("Canvas widget setup completed");
}

void MainWindow::update_preview() {
    if (canvas_widget_) {
        canvas_widget_->refresh();
    }
}

void MainWindow::encode_and_push() {
    if (!stream_pusher_ || !stream_pusher_->is_pushing()) {
        return;
    }

    // Video
    if (auto video_frame = video_engine_->get_latest_frame()) {
        std::vector<EncodedPacketPtr> packets;
        ErrorCode result = encoder_->encode_video_frame(video_frame, packets);
        if (result == ErrorCode::SUCCESS) {
            for (auto& p : packets) {
                if (!p) continue;
                p->wallclock_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                stream_pusher_->push_packet(p);
            }
        }
    }

    // Audio
    if (auto audio_frame = audio_engine_->get_audio_frame()) {
        std::vector<EncodedPacketPtr> packets;
        ErrorCode result = encoder_->encode_audio_frame(audio_frame, packets);
        if (result == ErrorCode::SUCCESS) {
            for (auto& p : packets) {
                if (!p) continue;
                p->wallclock_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                stream_pusher_->push_packet(p);
            }
        }
    }
}

void MainWindow::update_scene_items() {
    build_scene_list();
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

} // namespace live_assistant
