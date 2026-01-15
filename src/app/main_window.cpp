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
#include "app/add_material_dialog.h"
#include <QStyle>

namespace live_assistant {

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow) {
    ui->setupUi(this);
    
    // 设置窗口标题
    setWindowTitle("LiveAssistant");
    
    // 初始化预览定时器（暂时不启动，进入直播间后再启动）
    preview_timer_ = new QTimer(this);
    connect(preview_timer_, &QTimer::timeout, this, &MainWindow::update_preview);
    
    // 设置UI连接
    setup_ui_connections();
    
    // 更新初始状态
    update_status("Ready");
    
    LOG_INFO("MainWindow created");
    
    // Note: inline material buttons and label are handled via the Add Material dialog/button.
}

MainWindow::~MainWindow() {
    LOG_INFO("MainWindow destroyed");
    delete ui;
}

void MainWindow::set_live_id(const QString& live_id) {
    live_id_ = live_id;
    LOG_INFO("Live ID set: " + live_id_.toStdString());
    
    // 初始化模块（仅在进入直播间时）
    initialize_modules();
    
    // 创建并设置CanvasWidget
    setup_canvas_widget();
    
    // TODO: 根据live_id加载直播配置
}

void MainWindow::initialize_modules() {
    // 创建模块实例
    scene_manager_ = std::make_shared<SceneManager>();
    video_engine_ = std::make_shared<VideoEngine>();
    audio_engine_ = std::make_shared<AudioEngine>();
    encoder_ = std::make_shared<Encoder>();
    stream_pusher_ = std::make_shared<StreamPusher>();

    // 创建合成器和编码桥接器
    compositor_ = std::make_shared<Compositor>();
    encoder_bridge_ = std::make_shared<CompositorEncoderBridge>();
    
    // Capture manager
    capture_manager_ = std::make_shared<CaptureManagerIface>();
    
    // 初始化模块
    video_engine_->initialize(1920, 1080, 30);
    audio_engine_->initialize(44100, 2);
    
    // 设置初始场景
    video_engine_->set_current_scene(scene_manager_->get_current_scene());
    
    // 使用默认配置初始化编码器
    VideoEncoderConfig video_config;
    video_config.width = 1920;
    video_config.height = 1080;
    video_config.fps = 30;
    video_config.bitrate = 2500000; // 2.5 Mbps
    video_config.gop = 30;
    video_config.b_frames_enabled = false; // 按要求禁用B帧
    encoder_->initialize_video_encoder(video_config);
    
    AudioEncoderConfig audio_config;
    audio_config.sample_rate = 44100;
    audio_config.channels = 2;
    audio_config.bitrate = 128000; // 128 Kbps
    encoder_->initialize_audio_encoder(audio_config);
    
    // 设置编码定时器
    encoding_timer_ = new QTimer(this);
    connect(encoding_timer_, &QTimer::timeout, this, &MainWindow::encode_and_push);
    encoding_timer_->start(33); // ~30fps
}

void MainWindow::setup_ui_connections() {
    // Material actions are provided via the Add Material dialog/button below.
    
    // 连接添加素材按钮 -> 打开网格对话框
    // Replace label with a real button to trigger dialog
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
                    case AddMaterialDialog::Selection::Image:
                        QMessageBox::information(this, "提示", "图片功能开发中...");
                        break;
                    case AddMaterialDialog::Selection::Video:
                        QMessageBox::information(this, "提示", "视频功能开发中...");
                        break;
                    case AddMaterialDialog::Selection::Whiteboard:
                        QMessageBox::information(this, "提示", "白板功能开发中...");
                        break;
                    case AddMaterialDialog::Selection::CloudDoc:
                        QMessageBox::information(this, "提示", "云文档功能开发中...");
                        break;
                    case AddMaterialDialog::Selection::More:
                        QMessageBox::information(this, "提示", "更多功能开发中...");
                        break;
                    default:
                        break;
                }
            }
        });
    }
    
    // 连接开始直播按钮
    connect(ui->pushButton_startLive, &QPushButton::clicked, this, [this]() {
        LOG_INFO("Start live button clicked");
        // TODO: 实现开始直播功能
        QMessageBox::information(this, "提示", "开始直播功能开发中...");
    });
    
    // 连接场景相关按钮
    connect(ui->pushButton_addScene, &QPushButton::clicked, this, [this]() {
        LOG_INFO("Add scene button clicked");
        // TODO: 实现添加场景功能
        QMessageBox::information(this, "提示", "添加场景功能开发中...");
    });
}

void MainWindow::on_camera_button_clicked() {
    LOG_INFO("Camera button clicked");
    
    if (is_camera_preview_) {
        // 如果已经在预览，停止预览
        stop_camera_preview();
        // Do not modify the camera button text/style here to allow multiple independent camera sources
    } else {
        // 如果没有在预览，显示摄像头选择对话框
        show_camera_selector();
    }
}

void MainWindow::show_camera_selector() {
    if (!video_engine_) {
        LOG_ERROR("Video engine not initialized");
        QMessageBox::warning(this, "错误", "视频引擎未初始化");
        return;
    }
    
    // 获取可用摄像头列表
    std::vector<std::string> cameras = video_engine_->get_available_cameras();
    
    if (cameras.empty()) {
        LOG_WARNING("No cameras found");
        QMessageBox::information(this, "提示", "未检测到摄像头设备");
        return;
    }
    
    // 创建摄像头设置对话框
    CameraSettingsDialog dialog(this);
    
    // 设置可用摄像头列表
    dialog.set_available_cameras(cameras);
    
    // 设置默认值
    dialog.set_resolution("640x360");
    dialog.set_fps(30);
    dialog.set_pixel_format("PIXEL_FORMAT_YUY2");
    
    // 显示对话框
    if (dialog.exec() == QDialog::Accepted) {
        // 获取选择的摄像头名称
        QString selected_camera = QString::fromStdString(dialog.get_camera_name());
        
        // 获取摄像头参数
        std::string resolution = dialog.get_resolution();
        int fps = dialog.get_fps();
        std::string pixel_format = dialog.get_pixel_format();
        bool mirror = dialog.is_mirror();
        bool corner_rounding = dialog.is_corner_rounding();
        
        // 设置摄像头参数
        if (video_engine_) {
            video_engine_->set_camera_resolution(resolution);
            video_engine_->set_camera_fps(fps);
            video_engine_->set_camera_pixel_format(pixel_format);
        }
        
        // 调用原有的摄像头选择处理逻辑
        on_select_camera(selected_camera);
    }
}

void MainWindow::on_select_camera(const QString& camera_name) {
    LOG_INFO("Selected camera: " + camera_name.toStdString());
    
    if (!video_engine_) {
        LOG_ERROR("Video engine not initialized");
        return;
    }
    
    // 选择摄像头
    bool result = video_engine_->select_camera(camera_name.toStdString());
    if (!result) {
        LOG_ERROR("Failed to select camera");
        QMessageBox::warning(this, "错误", "选择摄像头失败");
        return;
    }
    
    // 设置捕获模式为 OpenCV（更稳定）
    video_engine_->set_capture_mode(VideoEngine::CaptureMode::OPENCV);
    
    // 开始摄像头预览
    start_camera_preview();
    
    // 创建摄像头Source对象并添加到场景中
    if (scene_manager_ && scene_manager_->get_current_scene()) {
        auto scene = scene_manager_->get_current_scene();
        
        // 创建摄像头源
        std::string camera_id = "camera_" + std::to_string(rand());
        auto camera_source = SourceFactory::create_camera_source(camera_id, camera_name.toStdString());
        
        // 初始化并启动摄像头源
        camera_source->initialize();
        camera_source->start();
        
        // 添加到场景中
        scene->add_source(camera_source);
        
        // 更新场景项列表
        update_scene_items();
        
        LOG_INFO("Added camera source to scene: " + camera_name.toStdString());
    }
}

void MainWindow::start_camera_preview() {
    if (!video_engine_) {
        LOG_ERROR("Video engine not initialized");
        return;
    }
    
    // 开始捕获
    bool result = video_engine_->start_capture();
    if (!result) {
        LOG_ERROR("Failed to start camera capture");
        QMessageBox::warning(this, "错误", "启动摄像头失败");
        return;
    }
    
    is_camera_preview_ = true;
    
    // 启动预览定时器
    preview_timer_->start(33); // ~30fps
    
    // 更新按钮状态
    // Do not change the camera button text/style; multiple cameras allowed
    
    LOG_INFO("Camera preview started");
}

void MainWindow::stop_camera_preview() {
    if (!video_engine_) {
        return;
    }
    
    // 停止捕获
    video_engine_->stop_capture();
    
    // 停止预览定时器
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

    // Create and show the screen capture selector dialog
    ScreenCaptureSelector dialog(this);

    if (dialog.exec() == QDialog::Accepted) {
        const auto* selected_target = dialog.get_selected_target();
        if (selected_target) {
            // For now, use default settings. Later we can show a settings dialog
            int fps = 30;
            QString resolution = "原尺寸";
            bool capture_cursor = true;
            bool capture_border = (selected_target->type == CaptureTarget::Type::WINDOW);

            // Create capture config and let factory produce a capture source (UI unaware of impl)
            CaptureConfig cfg;
            cfg.type = selected_target->type == CaptureTarget::Type::SCREEN ? CaptureConfig::TargetType::SCREEN : CaptureConfig::TargetType::WINDOW;
            cfg.target_id = selected_target->id;
            cfg.fps = fps;
            cfg.capture_cursor = capture_cursor;
            cfg.capture_border = capture_border;

            auto src = CaptureFactory::create_capture_source(cfg);
            if (src) {
                std::string source_id = std::string("capture_") + selected_target->id;

                // Add layer to compositor for this capture source
                if (compositor_) {
                    compositor_->add_layer(source_id);
                    compositor_->update_layer_transform(source_id, QRectF(50, 50, 640, 360)); // Default position and size
                    compositor_->set_layer_visible(source_id, true);
                }
                // set frame callback to update compositor layer
                // We will also forward frames to the Scene's ScreenSource (if present)
                std::weak_ptr<Scene> weak_scene;
                if (scene_manager_ && scene_manager_->get_current_scene()) {
                    weak_scene = scene_manager_->get_current_scene();
                }

                src->set_frame_callback([this, source_id, weak_scene](const CaptureFrame& frame) {
                    LOG_INFO("Capture frame from " + source_id + ", size: " +
                             std::to_string(frame.width) + "x" + std::to_string(frame.height));

                    // Update compositor layer with the captured image
                    if (compositor_ && !frame.image.isNull()) {
                        compositor_->update_layer_image(source_id, frame.image);
                    }
                    // Try to push frame into Scene's ScreenSource so CanvasRenderer can draw it
                    if (!weak_scene.expired()) {
                        auto scene = weak_scene.lock();
                        if (scene) {
                            auto items = scene->get_all_scene_items();
                            for (auto &it : items) {
                                auto srcPtr = it->get_source();
                                if (srcPtr && srcPtr->get_id() == source_id) {
                                    // dynamic_cast to ScreenSource if possible
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
                
                // Also create a ScreenSource placeholder and add to current scene for canvas rendering
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
            } else {
                LOG_ERROR("CaptureFactory failed to create source, using fallback preview only");
            }

            // Keep the existing UX message
            on_select_screen_share(
                QString::fromStdString(selected_target->id),
                selected_target->type == CaptureTarget::Type::SCREEN,
                fps,
                resolution,
                capture_cursor,
                capture_border
            );
        }
    }
}

void MainWindow::on_select_screen_share(const QString& target_id, bool is_screen_mode, int fps, const QString& resolution, bool capture_cursor, bool capture_border) {
    LOG_INFO("Selected screen share target: " + target_id.toStdString());

    QString target_type = is_screen_mode ? "屏幕" : "窗口";

    // Determine source id used when creating capture (match show_screen_share_selector)
    std::string source_id = std::string("capture_") + target_id.toStdString();

    // If we already created and started a capture source via CaptureFactory/CaptureManager,
    // report success; otherwise inform user that native WGC will be used when available.
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
    // 这个方法可以用于在帧准备好时执行某些操作
    // 目前由 update_preview() 处理
}

void MainWindow::update_status(const QString& message) {
    ui->statusBar->showMessage(message);
}

void MainWindow::setup_canvas_widget() {
    // 创建CanvasWidget实例
    canvas_widget_ = new CanvasWidget(this);
    
    // 设置场景管理器
    canvas_widget_->set_scene_manager(scene_manager_);
    
    // 设置视频引擎，用于显示摄像头帧
    canvas_widget_->set_video_engine(video_engine_);
    
    // 设置画布分辨率（默认1080p）
    canvas_widget_->set_canvas_resolution(1920, 1080);

    // 设置合成器
    canvas_widget_->set_compositor(compositor_);

    // 设置编码桥接器
    encoder_bridge_->set_compositor(compositor_);
    encoder_bridge_->set_encoder(encoder_);
    encoder_bridge_->set_stream_pusher(stream_pusher_);
    encoder_bridge_->set_resolution(1920, 1080);

    // 用我们的CanvasWidget替换label_livePreview
    // 从布局中移除现有组件
    ui->verticalLayout_liveArea->removeWidget(ui->label_livePreview);
    delete ui->label_livePreview;
    ui->label_livePreview = nullptr;
    
    // 将CanvasWidget添加到布局
    ui->verticalLayout_liveArea->insertWidget(0, canvas_widget_);
    
    // 连接信号
    connect(canvas_widget_, &CanvasWidget::scene_item_selected, this, [this](std::shared_ptr<SceneItem> item) {
        LOG_INFO("Scene item selected: " + (item ? item->get_source_id() : "null"));
    });
    
    LOG_INFO("Canvas widget setup completed");
}

void MainWindow::update_preview() {
    // 刷新画布组件
    if (canvas_widget_) {
        canvas_widget_->refresh();
    }
    
    // 如果正在预览摄像头，将视频帧添加到场景中
    if (is_camera_preview_ && video_engine_ && scene_manager_) {
        // 获取最新的视频帧
        auto frame = video_engine_->get_latest_frame();
        if (frame) {
            // TODO: 将视频帧添加到场景中
            // 这里需要实现将VideoEngine的帧添加到场景的逻辑
            // 目前先记录日志
            LOG_INFO("Received camera frame: " + std::to_string(frame->width) + "x" + std::to_string(frame->height));
        }
    }
}

void MainWindow::encode_and_push() {
    // 检查是否处于推送状态
    if (!stream_pusher_->is_pushing()) {
        return;
    }
    
    // 1. 从视频引擎获取视频帧
    auto video_frame = video_engine_->get_latest_frame();
    if (video_frame) {
        // 2. 编码视频帧
        std::vector<uint8_t> encoded_video;
        ErrorCode result = encoder_->encode_video_frame(video_frame, encoded_video);
        if (result == ErrorCode::SUCCESS && !encoded_video.empty()) {
            // 3. 创建视频媒体包
            MediaPacket video_packet;
            video_packet.type = MediaType::VIDEO;
            video_packet.timestamp = video_frame->timestamp_ms * 1000; // 转换为微秒
            video_packet.data = encoded_video;
            video_packet.is_keyframe = (rand() % 30 == 0); // 每约30帧模拟一个关键帧
            video_packet.is_config = false;
            
            // 根据关键帧设置视频类型
            if (video_packet.is_keyframe) {
                video_packet.video_type = VideoFrameType::I_FRAME;
            } else {
                video_packet.video_type = VideoFrameType::P_FRAME;
            }
            
            // 设置优先级（音频优先级高于视频）
            video_packet.priority = 1;
            
            // 4. 将视频包推送到流推送器
            stream_pusher_->push_packet(video_packet);
        }
    }
    
    // 1. 从音频引擎获取音频帧
    auto audio_frame = audio_engine_->get_audio_frame();
    if (audio_frame) {
        // 2. 编码音频帧
        std::vector<uint8_t> encoded_audio;
        ErrorCode result = encoder_->encode_audio_frame(audio_frame, encoded_audio);
        if (result == ErrorCode::SUCCESS && !encoded_audio.empty()) {
            // 3. 创建音频媒体包
            MediaPacket audio_packet;
            audio_packet.type = MediaType::AUDIO;
            audio_packet.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            audio_packet.data = encoded_audio;
            audio_packet.is_keyframe = false;
            audio_packet.is_config = false;
            
            // 设置优先级（音频优先级高于视频）
            audio_packet.priority = 2;
            
            // 4. 将音频包推送到流推送器
            stream_pusher_->push_packet(audio_packet);
        }
    }
}

void MainWindow::toggle_scene_item_visibility(int index) {
    if (!scene_manager_ || !scene_manager_->get_current_scene()) {
        LOG_ERROR("Scene manager or current scene not initialized");
        return;
    }
    
    auto scene = scene_manager_->get_current_scene();
    auto scene_items = scene->get_all_scene_items();
    
    if (index < 0 || index >= scene_items.size()) {
        LOG_ERROR("Invalid scene item index: " + std::to_string(index));
        return;
    }
    
    auto item = scene_items[index];
    bool new_visibility = !item->is_visible();
    item->set_visible(new_visibility);
    
    // 更新场景项列表UI
    update_scene_items();
    
    LOG_INFO("Toggled scene item visibility: " + item->get_source_id() + " to " + (new_visibility ? "visible" : "hidden"));
    
    // 刷新画布
    if (canvas_widget_) {
        canvas_widget_->refresh();
    }
}

void MainWindow::show_scene_item_settings(int index) {
    if (!scene_manager_ || !scene_manager_->get_current_scene()) {
        LOG_ERROR("Scene manager or current scene not initialized");
        return;
    }
    
    auto scene = scene_manager_->get_current_scene();
    auto scene_items = scene->get_all_scene_items();
    
    if (index < 0 || index >= scene_items.size()) {
        LOG_ERROR("Invalid scene item index: " + std::to_string(index));
        return;
    }
    
    auto item = scene_items[index];
    auto source = item->get_source();
    LOG_INFO("Opening settings for scene item: " + item->get_source_id());
    
    // 创建摄像头设置对话框
    CameraSettingsDialog dialog(this);
    
    // 设置对话框默认值
    std::string metadata = source->get_metadata();
    
    // 从metadata中提取信息
    size_t name_pos = metadata.find("name:");
    if (name_pos != std::string::npos) {
        size_t end_pos = metadata.find(",", name_pos + 5);
        if (end_pos != std::string::npos) {
            dialog.set_camera_name(metadata.substr(name_pos + 5, end_pos - name_pos - 5));
        } else {
            dialog.set_camera_name(metadata.substr(name_pos + 5));
        }
    }
    
    // 设置默认分辨率、帧率等
    dialog.set_resolution("640x360");
    dialog.set_fps(30);
    dialog.set_pixel_format("PIXEL_FORMAT_YUY2");
    
    // 显示对话框
    if (dialog.exec() == QDialog::Accepted) {
        // 应用设置
        LOG_INFO("Camera settings applied");
        LOG_INFO("New camera name: " + dialog.get_camera_name());
        LOG_INFO("New resolution: " + dialog.get_resolution());
        LOG_INFO("New fps: " + std::to_string(dialog.get_fps()));
        LOG_INFO("New pixel format: " + dialog.get_pixel_format());
        LOG_INFO("Mirror: " + std::string(dialog.is_mirror() ? "true" : "false"));
        LOG_INFO("Camera enabled: " + std::string(dialog.is_camera_enabled() ? "true" : "false"));
        LOG_INFO("Corner rounding: " + std::string(dialog.is_corner_rounding() ? "true" : "false"));
        
        // TODO: 将设置应用到摄像头源
    }
}

void MainWindow::delete_scene_item(int index) {
    if (!scene_manager_ || !scene_manager_->get_current_scene()) {
        LOG_ERROR("Scene manager or current scene not initialized");
        return;
    }
    
    auto scene = scene_manager_->get_current_scene();
    auto scene_items = scene->get_all_scene_items();
    
    if (index < 0 || index >= scene_items.size()) {
        LOG_ERROR("Invalid scene item index: " + std::to_string(index));
        return;
    }
    
    auto item = scene_items[index];
    LOG_INFO("Deleting scene item: " + item->get_source_id());
    
    // 从场景中移除
    // If it's a ScreenSource, clear its latest frame first to avoid leftover image
    auto source = item->get_source();
    if (source) {
        auto screenSrc = std::dynamic_pointer_cast<ScreenSource>(source);
        if (screenSrc) {
            screenSrc->push_frame(QImage()); // clear
            LOG_INFO("Cleared ScreenSource latest frame for: " + source->get_id());
        }
    }
    scene->remove_scene_item(item);

    // Also remove associated capture source and compositor layer if present
    std::string sid = item->get_source_id();
    if (capture_manager_ && capture_manager_->has_source(sid)) {
        capture_manager_->stop_source(sid);
        capture_manager_->remove_source(sid);
        LOG_INFO("Removed capture manager source: " + sid);
    }

    if (compositor_ && compositor_->has_layer(sid)) {
        compositor_->remove_layer(sid);
        LOG_INFO("Removed compositor layer: " + sid);
    }

    // If this was a camera preview source, stop preview if no other camera previews exist
    if (sid.rfind("camera_", 0) == 0) {
        // stop camera preview UI and engine
        if (is_camera_preview_) {
            stop_camera_preview();
        }
    }
    
    // 更新场景项列表
    update_scene_items();
    
    // 刷新画布
    if (canvas_widget_) {
        canvas_widget_->refresh();
    }
}

void MainWindow::update_scene_items() {
    if (!scene_manager_ || !scene_manager_->get_current_scene()) {
        LOG_ERROR("Scene manager or current scene not initialized");
        return;
    }
    
    // 清空现有的场景项UI
    QLayoutItem* child;
    while ((child = ui->verticalLayout_sceneItems->takeAt(0)) != nullptr) {
        // 删除布局和其中的控件
        QWidget* widget = child->widget();
        delete child;
        delete widget;
    }
    
    // 获取当前场景的所有场景项
    auto scene = scene_manager_->get_current_scene();
    auto scene_items = scene->get_all_scene_items();
    
    const int total = static_cast<int>(scene_items.size());
    // 按 order 降序展示（新添加的在列表顶部）
    for (int displayIdx = 0; displayIdx < total; ++displayIdx) {
        int realIdx = total - 1 - displayIdx; // real index in ascending array
        auto item = scene_items[realIdx];
        auto source = item->get_source();

        // 创建水平布局
        QHBoxLayout* layout = new QHBoxLayout();
        layout->setSpacing(5);
        layout->setContentsMargins(5, 5, 5, 5);

        // 创建名称按钮
        QString source_name = extract_source_name(source);
        QPushButton* name_button = create_scene_item_button("■ " + source_name,
            "text-align: left; border: none; background: transparent;");

        // 创建按钮（使用标准图标）
        QPushButton* move_up_button = create_icon_button(QStyle::SP_ArrowUp, "Move Up");
        move_up_button->setEnabled(displayIdx > 0); // 顶部置灰

        QPushButton* eye_button = create_icon_button(item->is_visible() ? QStyle::SP_DialogYesButton
                                                                      : QStyle::SP_DialogNoButton,
                                                     item->is_visible() ? "Hide" : "Show");
        QPushButton* settings_button = create_icon_button(QStyle::SP_FileDialogDetailedView, "Settings");
        QPushButton* delete_button = create_icon_button(QStyle::SP_TrashIcon, "Delete");

        // 连接信号
        connect(eye_button, &QPushButton::clicked, this, [this, realIdx]() {
            toggle_scene_item_visibility(realIdx);
        });

        connect(settings_button, &QPushButton::clicked, this, [this, realIdx]() {
            show_scene_item_settings(realIdx);
        });

        connect(delete_button, &QPushButton::clicked, this, [this, realIdx]() {
            delete_scene_item(realIdx);
        });

        // 连接上移层级
        connect(move_up_button, &QPushButton::clicked, this, [this, item]() {
            if (!scene_manager_ || !scene_manager_->get_current_scene()) return;
            auto scene = scene_manager_->get_current_scene();
            scene->move_scene_item_down(item);
            update_scene_items();
            if (canvas_widget_) {
                canvas_widget_->refresh();
            }
        });

        // 添加控件到布局
        layout->addWidget(name_button, 1); // 名称按钮占据大部分空间
        layout->addWidget(move_up_button);
        layout->addWidget(eye_button);
        layout->addWidget(settings_button);
        layout->addWidget(delete_button);

        // 创建QWidget并设置布局
        QWidget* item_widget = new QWidget();
        item_widget->setLayout(layout);

        // 添加到场景项列表
        ui->verticalLayout_sceneItems->addWidget(item_widget);
    }
    
    LOG_INFO("Updated scene items UI, count: " + std::to_string(scene_items.size()));
}

// Helper methods for UI components
QString MainWindow::extract_source_name(std::shared_ptr<Source> source) {
    if (!source) return "Unknown";

    // 这里可以根据不同的source类型返回不同的名称
    // 暂时返回一个通用名称
    // Prefer metadata name if available
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

    // Fallback: infer by type and id
    switch (source->get_type()) {
        case Source::Type::VIDEO_CAPTURE:
            return QString::fromStdString(source->get_id());
        case Source::Type::SCREEN_CAPTURE:
            return QString::fromStdString(source->get_id());
        default:
            break;
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
