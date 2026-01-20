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
#include <QInputDialog>
#include <QHBoxLayout>
#include <QStyle>
#include <QAbstractItemModel>
#include <QVariant>

#include <unordered_set>

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

            LOG_INFO("[DIAG] 检查推流器和编码器是否初始化");
            if (!stream_pusher_ || !encoder_) {
                LOG_ERROR("[DIAG] 推流器或编码器未初始化");
                QMessageBox::warning(this, "错误", "推流器或编码器未初始化");
                return;
            }
            LOG_INFO("[DIAG] 推流器和编码器已初始化");

            if (stream_pusher_->is_pushing()) {
                LOG_INFO("[DIAG] 当前正在推流，准备停止推流");
                if (encoder_bridge_) {
                    LOG_INFO("[DIAG] 停止encoder_bridge");
                    encoder_bridge_->stop();
                }
                LOG_INFO("[DIAG] 停止stream_pusher");
                stream_pusher_->stop();
                streams_registered_ = false;
                LOG_INFO("[DIAG] 推流已停止");
                QMessageBox::information(this, "提示", "已停止推流");
                return;
            }

            LOG_INFO("[DIAG] 准备开始推流");
            LOG_INFO("[DIAG] 创建推流配置");
            StreamConfig cfg;
            cfg.server_url = rtmp_server_url_.isEmpty() ? "rtmp://localhost/live" : rtmp_server_url_.toStdString();
            cfg.stream_key = rtmp_stream_key_.isEmpty() ? "test" : rtmp_stream_key_.toStdString();
            cfg.max_queue_size = 300;
            cfg.auto_reconnect = true;
            cfg.low_latency = true;
            LOG_INFO("[DIAG] 推流配置: 服务器URL=" + cfg.server_url + ", 流密钥=" + cfg.stream_key + ", 最大队列大小=" + std::to_string(cfg.max_queue_size));

            LOG_INFO("[DIAG] 设置推流配置");
            ErrorCode cfg_ret = stream_pusher_->set_config(cfg);
            if (cfg_ret != ErrorCode::SUCCESS) {
                LOG_ERROR("[DIAG] 设置推流配置失败，错误码: " + std::to_string(static_cast<int>(cfg_ret)));
                QMessageBox::warning(this, "错误", "设置推流配置失败");
                return;
            }
            LOG_INFO("[DIAG] 推流配置设置成功");

            if (!streams_registered_) {
                LOG_INFO("[DIAG] 音视频流未注册，准备注册");
                LOG_INFO("[DIAG] 获取音频编码参数和时间基");
                AVCodecParameters* a_par = encoder_->get_audio_codec_parameters();
                AVRational a_tb = encoder_->get_audio_time_base();
                LOG_INFO("[DIAG] 获取视频编码参数和时间基");
                AVCodecParameters* v_par = encoder_->get_video_codec_parameters();
                AVRational v_tb = encoder_->get_video_time_base();

                if (!a_par || a_tb.den <= 0 || !v_par || v_tb.den <= 0) {
                    LOG_ERROR("[DIAG] 音频/视频编码器参数不可用");
                    if (a_par) avcodec_parameters_free(&a_par);
                    if (v_par) avcodec_parameters_free(&v_par);
                    QMessageBox::warning(this, "错误", "音频/视频编码器参数不可用（可能 H264 encoder 不存在）");
                    return;
                }
                LOG_INFO("[DIAG] 音视频编码参数获取成功");

                LOG_INFO("[DIAG] 注册音频流");
                ErrorCode ra = stream_pusher_->register_audio_stream(a_par, a_tb);
                LOG_INFO("[DIAG] 注册视频流");
                ErrorCode rv = stream_pusher_->register_video_stream(v_par, v_tb);

                avcodec_parameters_free(&a_par);
                avcodec_parameters_free(&v_par);

                if (ra != ErrorCode::SUCCESS || rv != ErrorCode::SUCCESS) {
                    LOG_ERROR("[DIAG] 注册音视频流失败，音频错误码: " + std::to_string(static_cast<int>(ra)) + ", 视频错误码: " + std::to_string(static_cast<int>(rv)));
                    QMessageBox::warning(this, "错误", "注册音视频流失败");
                    return;
                }
                LOG_INFO("[DIAG] 音视频流注册成功");
                streams_registered_ = true;
            } else {
                LOG_INFO("[DIAG] 音视频流已注册，跳过注册步骤");
            }

            // Ensure audio capture running before pushing.
            LOG_INFO("[DIAG] 检查音频引擎是否存在");
            if (audio_engine_) {
                LOG_INFO("[DIAG] 启动音频捕获");
                audio_engine_->start_capture();
            }

            LOG_INFO("[DIAG] 启动推流器");
            ErrorCode start_ret = stream_pusher_->start();
            if (start_ret != ErrorCode::SUCCESS) {
                LOG_ERROR("[DIAG] 启动推流失败，错误码: " + std::to_string(static_cast<int>(start_ret)));
                QMessageBox::warning(this, "错误", "启动推流失败");
                streams_registered_ = false;
                return;
            }
            LOG_INFO("[DIAG] 推流器启动成功");

            // Sync compositor to match Scene layout before first encoded frame.
            LOG_INFO("[DIAG] 在第一个编码帧之前同步compositor和场景布局");
            sync_scene_to_compositor();

            // Request keyframe on start if supported.
            // (Encoder currently doesn't expose force_keyframe; kept as a TODO for future.)

            LOG_INFO("[DIAG] 检查encoder_bridge是否存在");
            if (encoder_bridge_) {
                LOG_INFO("[DIAG] 启动encoder_bridge，帧率: " + std::to_string(encoder_->get_video_config().fps));
                encoder_bridge_->start(encoder_->get_video_config().fps);
            }

            LOG_INFO("[DIAG] 推流已成功开始");
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

        compositor_->update_layer_transform(sid, QRectF(tr.x, tr.y, w, h), tr.opacity);
        compositor_->set_layer_visible(sid, it->is_visible());
        compositor_->set_layer_order(sid, it->get_order());
    }

    for (const auto& lid : compositor_->get_layer_ids()) {
        if (active.find(lid) == active.end()) {
            compositor_->remove_layer(lid);
        }
    }
}

} // namespace live_assistant
