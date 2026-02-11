#include "app/main_window.h"
#include "app/camera_settings.h"
#include "app/screen_share.h"
#include "app/screen_capture_selector.h"
#include "app/settings_dialog.h"
#include "app/exit_dialog.h"
#include "app/insert_video_widget.h"
#include "app/insert_file_manager.h"
#include "app/add_material_dialog.h"
#include "http/network_manager.h"
#include "http/live_item.h"
#include "media_pipeline/media_file_source.h"
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
#include "encoder/encoder.h"
#include "stream_pusher/stream_pusher.h"
#include "common/log.h"
#include "common/error.h"

#include <QWindow>
#include <QTimer>
#include <QImage>
#include <QPixmap>
#include <QMessageBox>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QDateTime>
#include <QInputDialog>
#include <QHBoxLayout>
#include <QProcess>
#include <QTextStream>
#include <QGuiApplication>
#include <QScreen>
#include <QFile>
#include <QCloseEvent>
#include <QSettings>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include <QMenu>
#include <QAction>
#include <QStyle>
#include <QAbstractItemModel>
#include <QVariant>
#include <QThread>

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

    // 加载QSS样式文件（仅应用于主窗口）
    QFile qssFile(":/resources/live_companion_style.qss");
    if (qssFile.open(QFile::ReadOnly | QFile::Text)) {
        QTextStream stream(&qssFile);
        QString qssContent = stream.readAll();
        this->setStyleSheet(qssContent);
        qssFile.close();
        LOG_INFO("QSS stylesheet loaded for MainWindow");
    } else {
        LOG_WARNING("Failed to load QSS stylesheet: " + qssFile.errorString().toStdString());
    }

    // 设置底部控制栏按钮样式 - 与开始直播按钮风格统一
    setupBottomButtonsStyle();

    setWindowTitle("LiveAssistant");
    // Enhance top bar title: replace simple text with logo + gradient title + italic suffix
    if (ui->topBar) {
        // create container
        QWidget* titleContainer = new QWidget(ui->topBar);
        titleContainer->setObjectName("titleContainer");
        QHBoxLayout* tlay = new QHBoxLayout(titleContainer);
        tlay->setContentsMargins(8, 4, 8, 4);
        tlay->setSpacing(8);

        // logo
        QLabel* logoLbl = new QLabel(titleContainer);
        logoLbl->setFixedSize(32, 32);  // 先设置固定尺寸
        QPixmap iconPix(":/images/Frame_icon.png");
        if (!iconPix.isNull()) {
            // 使用KeepAspectRatioByExpanding确保填满32x32区域
            QPixmap scaledPix = iconPix.scaled(32, 32, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            logoLbl->setPixmap(scaledPix);
        }
        tlay->addWidget(logoLbl);

        // gradient text pixmap for main title
        QString mainText = QString::fromUtf8("直播伴侣");
        QFont tf = ui->label_title ? ui->label_title->font() : this->font();
        tf.setPointSize(14);
        tf.setBold(true);
        QPainterPath path;
        path.addText(0, 0, tf, mainText);
        QRectF br = path.boundingRect();
        QPixmap titlePix(int(br.width()) + 4, int(br.height()) + 4);
        titlePix.fill(Qt::transparent);
        {
            QPainter p(&titlePix);
            p.setRenderHint(QPainter::Antialiasing);
            p.translate(-br.left(), -br.top());
            QLinearGradient lg(0, 0, br.width(), 0);
            lg.setColorAt(0.0, QColor(255, 106, 106));
            lg.setColorAt(1.0, QColor(74, 110, 240));
            p.fillPath(path, QBrush(lg));
        }
        QLabel* titleLbl = new QLabel(titleContainer);
        titleLbl->setPixmap(titlePix);
        titleLbl->setFixedSize(titlePix.size());
        tlay->addWidget(titleLbl);

        // suffix
        QLabel* suffix = new QLabel(QString::fromUtf8("·启点点"), titleContainer);
        QFont suf = tf;
        suf.setPointSize(11);
        suf.setItalic(true);
        suffix->setFont(suf);
        tlay->addWidget(suffix);

        titleContainer->setLayout(tlay);

        // Insert into topBar layout replacing existing label_title
        if (ui->topBar->layout()) {
            QHBoxLayout* hl = qobject_cast<QHBoxLayout*>(ui->topBar->layout());
            if (hl) {
                // find index of existing label_title if present
                int index = -1;
                for (int i = 0; i < hl->count(); ++i) {
                    QLayoutItem* it = hl->itemAt(i);
                    if (!it) continue;
                    if (it->widget() == ui->label_title) { index = i; break; }
                }
                if (index >= 0) {
                    // remove old label_title widget from layout and hide it
                    QWidget* old = ui->label_title;
                    hl->removeWidget(old);
                    old->hide();
                    hl->insertWidget(index, titleContainer);
                } else {
                    hl->insertWidget(0, titleContainer);
                }
            }
        }
    }

    // Make window frameless and use our custom topBar as title bar
    setWindowFlag(Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    
    // Install event filter on topBar to enable window dragging
    if (ui->topBar) {
        ui->topBar->installEventFilter(this);
        ui->topBar->setAttribute(Qt::WA_Hover, true);
    }
    
    // connect window control buttons if present
    if (ui->pushButton_minimize) {
        connect(ui->pushButton_minimize, &QPushButton::clicked, this, &MainWindow::showMinimized);
    }
    if (ui->pushButton_maximize) {
        connect(ui->pushButton_maximize, &QPushButton::clicked, this, [this]() {
            // Top bar maximize should control the whole main window (not the stage).
            if (isMaximized()) this->showNormal(); else this->showMaximized();
        });
    }
    if (ui->pushButton_close) {
        connect(ui->pushButton_close, &QPushButton::clicked, this, &MainWindow::handleExit);
    }

    preview_timer_ = new QTimer(this);
    connect(preview_timer_, &QTimer::timeout, this, &MainWindow::update_preview);

    // Replace simple preview label with a styled stage container (visual placeholder)
    if (ui->label_livePreview && ui->verticalLayout_liveArea) {
        // We're going to create a fixed-size placeholder in the layout (to reserve space)
        // and create the real stage container as a child of the liveArea so it can be moved.
        QWidget* layoutPlaceholder = new QWidget(this);
        layoutPlaceholder->setObjectName("stageLayoutPlaceholder");
        layoutPlaceholder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

        // let the placeholder expand to fill available space (we'll fit canvas inside)
        layoutPlaceholder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

        QBoxLayout* vlay = qobject_cast<QBoxLayout*>(ui->verticalLayout_liveArea);
        int insertIndex = -1;
        if (vlay) {
            for (int i = 0; i < vlay->count(); ++i) {
                QLayoutItem* it = vlay->itemAt(i);
                if (!it) continue;
                if (it->widget() == ui->label_livePreview) { insertIndex = i; break; }
            }
            if (insertIndex >= 0) {
                QWidget* old = ui->label_livePreview;
                vlay->removeWidget(old);
                old->hide();
                vlay->insertWidget(insertIndex, layoutPlaceholder);
            } else {
                vlay->addWidget(layoutPlaceholder);
            }
        } else {
            ui->verticalLayout_liveArea->addWidget(layoutPlaceholder);
        }

        // create stage container as a child of the layout placeholder so it follows layout sizing
        QWidget* stageContainer = new QWidget(layoutPlaceholder);
        stageContainer->setObjectName("liveViewWidget");
        stageContainer->setAttribute(Qt::WA_StyledBackground, true);

        QVBoxLayout* scLayout = new QVBoxLayout(stageContainer);
        scLayout->setContentsMargins(0, 0, 0, 0);
        scLayout->setSpacing(6);

        // center placeholder icon and text
        QLabel* placeholderIcon = new QLabel(stageContainer);
        placeholderIcon->setText(QString::fromUtf8("✚"));
        placeholderIcon->setObjectName("labelAddLive");
        QFont iconFont = placeholderIcon->font();
        iconFont.setPointSize(36);
        placeholderIcon->setFont(iconFont);
        placeholderIcon->setAlignment(Qt::AlignCenter);

        QLabel* placeholderText = new QLabel(QString::fromUtf8("添加直播画面"), stageContainer);
        placeholderText->setObjectName("labelAddLive");
        QFont txtF = placeholderText->font();
        txtF.setPointSize(16);
        placeholderText->setFont(txtF);
        placeholderText->setAlignment(Qt::AlignCenter);

        scLayout->addStretch();
        scLayout->addWidget(placeholderIcon);
        scLayout->addWidget(placeholderText);
        scLayout->addStretch();
        stageContainer->setLayout(scLayout);

        // position stageContainer centered within layoutPlaceholder
        QRect lpRect = layoutPlaceholder->geometry();
        QPoint topLeft = layoutPlaceholder->mapTo(ui->liveArea, QPoint(0,0));
        // position stageContainer to exactly fill the layoutPlaceholder immediately
        stageContainer->setGeometry(0, 0, layoutPlaceholder->width(), layoutPlaceholder->height());

        // Save pointers for later updates
        stagePlaceholderWidget_ = layoutPlaceholder;
        stageContainer_ = stageContainer;
        placeholderIcon_ = placeholderIcon;
        placeholderText_ = placeholderText;

        // Add an overlay button in the center to handle "add source" clicks
        stageAddButton_ = new QPushButton(stageContainer_);
        stageAddButton_->setText("");
        stageAddButton_->setCursor(Qt::PointingHandCursor);
        stageAddButton_->setFlat(true);
        stageAddButton_->setGeometry(stageContainer_->rect());
        stageAddButton_->show();
        stageAddButton_->raise();
        // forward to the right-side add material logic so behavior is identical
        if (ui->pushButton_addMaterial) {
            ui->pushButton_addMaterial->setObjectName("btnAddMaterial");
            connect(stageAddButton_, &QPushButton::clicked, this, [this]() {
                ui->pushButton_addMaterial->click();
            });
        } else {
            connect(stageAddButton_, &QPushButton::clicked, this, [this]() {
                AddMaterialDialog dlg(this);
                dlg.exec();
            });
        }

        // No per-stage control buttons: stage is always filled and controlled by main window.

        // install event filters so we can keep overlay geometry and aspect ratio in sync and support resizing
        stageContainer_->installEventFilter(this);
        if (stagePlaceholderWidget_) stagePlaceholderWidget_->installEventFilter(this);
        if (ui && ui->liveArea) ui->liveArea->installEventFilter(this);
        // set initial aspect height based on liveArea width
        if (ui && ui->liveArea) {
            // size will be managed by layouts; ensure stageContainer matches placeholder
            stageContainer_->setGeometry(0, 0, stagePlaceholderWidget_->width(), stagePlaceholderWidget_->height());
            stageAddButton_->setGeometry(stageContainer_->rect());
            if (canvas_widget_) canvas_widget_->setGeometry(stageContainer_->rect());
        }
        // Initialize placeholder visibility
        updateStagePlaceholderVisibility();
    }

    setup_ui_connections();
    setup_scene_list();

    // 加载退出偏好设置
    loadExitPreference();

    // 初始化系统托盘图标
    setupSystemTray();

    // 初始化网络连接
    setupNetworkConnections();

    update_status("Ready");

    // DPI 适配：监听屏幕 DPI 变化（当窗口在不同屏幕间移动时）
    connect(windowHandle(), &QWindow::screenChanged, this, [this](QScreen* screen) {
        if (screen) {
            // 当屏幕变化时，更新窗口的 DPI 缩放
            LOG_INFO("Screen changed, DPI: " + std::to_string(screen->logicalDotsPerInch()));
            // 触发窗口更新以应用新的 DPI
            this->updateGeometry();
        }
    });

    // 监听当前屏幕的 DPI 变化
    if (windowHandle() && windowHandle()->screen()) {
        connect(windowHandle()->screen(), &QScreen::logicalDotsPerInchChanged, this, [this](qreal dpi) {
            LOG_INFO("DPI changed to: " + std::to_string(dpi));
            this->updateGeometry();
        });
    }

    LOG_INFO("MainWindow created");
}

MainWindow::~MainWindow() {
    LOG_INFO("MainWindow destroyed");

    // 清理系统托盘
    cleanupSystemTray();

    // Stop timers
    if (stats_update_timer_) {
        stats_update_timer_->stop();
    }
    if (system_info_timer_) {
        system_info_timer_->stop();
    }

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

    // 先分离摄像头和其他场景项
    std::vector<std::shared_ptr<SceneItem>> camera_items;
    std::vector<std::shared_ptr<SceneItem>> other_items;
    
    for (const auto& item : scene_items) {
        if (QString::fromStdString(item->get_source_id()).startsWith("camera_")) {
            camera_items.push_back(item);
        } else {
            other_items.push_back(item);
        }
    }
    
    // 先添加摄像头项，确保它们在列表顶部
    for (const auto& item : camera_items) {
        auto* lw_item = new QListWidgetItem(listWidget_sceneItems_);
        lw_item->setSizeHint(QSize(240, 34));
        lw_item->setData(Qt::UserRole, QString::fromStdString(item->get_source_id()));

        QString display_name = extract_source_name(item->get_source());
        auto* row = new SceneItemRow(item, display_name, listWidget_sceneItems_);

        const int row_index = listWidget_sceneItems_->row(lw_item);
        row->set_move_up_enabled(false); // 摄像头项不能再往上移动

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

        // 摄像头项不需要上移功能

        listWidget_sceneItems_->setItemWidget(lw_item, row);
    }
    
    // 再添加其他场景项
    const int total = static_cast<int>(other_items.size());
    for (int i = total - 1; i >= 0; --i) {
        auto item = other_items[i];

        auto* lw_item = new QListWidgetItem(listWidget_sceneItems_);
        lw_item->setSizeHint(QSize(240, 34));
        lw_item->setData(Qt::UserRole, QString::fromStdString(item->get_source_id()));

        QString display_name = extract_source_name(item->get_source());
        auto* row = new SceneItemRow(item, display_name, listWidget_sceneItems_);

        const int row_index = listWidget_sceneItems_->row(lw_item);
        row->set_move_up_enabled(row_index > 0); // 其他项可以上移，但不能超过摄像头项

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
            // 检查目标位置是否是摄像头项
            auto* target_item = listWidget_sceneItems_->item(r - 1);
            QString target_sid = target_item->data(Qt::UserRole).toString();
            if (target_sid.startsWith("camera_")) {
                return; // 不能移动到摄像头项上面
            }
            listWidget_sceneItems_->model()->moveRow(QModelIndex(), r, QModelIndex(), r - 1);
        });

        listWidget_sceneItems_->setItemWidget(lw_item, row);
    }
    // update placeholder visibility after rebuilding scene list
    updateStagePlaceholderVisibility();
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
                // 如果是摄像头源，保持其较高的order值
                if (!sid.startsWith("camera_")) {
                    it->set_order(order);
                } else {
                    // 摄像头源保持较高的order值
                    it->set_order(9999);
                }
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

void MainWindow::setCredentials(const QString& socketUrl, const QString& userId, const QString& token, const QString& liveurl, const QString& oncekey) {
    socket_url_ = socketUrl;
    user_id_ = userId;
    token_ = token;
    live_url_ = liveurl;
    once_key_ = oncekey;

    LOG_INFO(QString("Credentials set - userId: %1, liveUrl: %2, socketUrl: %3")
        .arg(userId).arg(liveurl).arg(socketUrl).toStdString());
}

void MainWindow::setLiveItem(const LiveItem& liveItem) {
    current_live_item_ = liveItem;

    LOG_INFO(QString("LiveItem set - liveId: %1, title: %2, status: %3")
        .arg(liveItem.liveId)
        .arg(liveItem.title)
        .arg(liveItem.status == LiveStatus::LIVE ? "直播中" :
            liveItem.status == LiveStatus::PENDING ? "待开播" : "已结束")
        .toStdString());

    // 如果有推流地址，自动设置RTMP目标
    if (!liveItem.pushUrl.isEmpty() && !liveItem.pushUrl[0].isEmpty()) {
        QString rtmpUrl = liveItem.pushUrl[0];
        // 解析RTMP地址（假设格式为 rtmp://server/app/stream_key）
        LOG_INFO(QString("Auto-set RTMP URL from LiveItem: %1").arg(rtmpUrl).toStdString());
        // 这里可以根据需要进一步解析 server_url 和 stream_key
        rtmp_server_url_ = rtmpUrl;
    }
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
    audio_engine_->initialize(48000, 2);
    // Default: start microphone capture when entering live room
    if (audio_engine_) {
        LOG_INFO("Starting audio capture by default for live room");
        update_audio_status("初始化中...","orange");

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
            update_audio_status("故障", "red");
            // enable silent audio fallback in encoder bridge
            if (encoder_bridge_) {
                encoder_bridge_->set_audio_engine(audio_engine_);
                encoder_bridge_->set_silent_audio(true);
            }
            QMessageBox::warning(this, "麦克风故障",
                "无法打开麦克风，程序将以静音推流作为回退。\n\n可能的原因：\n• 麦克风被其他程序占用\n• 音频设备驱动问题\n• 系统音频服务未运行\n\n请尝试：\n1. 检查麦克风是否被其他程序使用\n2. 重新启动应用程序\n3. 检查音频设备设置");
        } else {
            update_audio_status("正常", "green");
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

    // 使用音频引擎的实际采样率和声道数（设备原生格式）
    AudioEncoderConfig audio_config;
    audio_config.sample_rate = audio_engine_->get_sample_rate();
    audio_config.channels = audio_engine_->get_channels();
    audio_config.bitrate = 128000;
    LOG_INFO("Audio encoder config: sample_rate=" + std::to_string(audio_config.sample_rate) +
             ", channels=" + std::to_string(audio_config.channels));
    encoder_->initialize_audio_encoder(audio_config);

    encoding_timer_ = new QTimer(this);
    connect(encoding_timer_, &QTimer::timeout, this, &MainWindow::encode_and_push);
    encoding_timer_->start(33);

    streams_registered_ = false;
    // 使用UI文件中定义的标签，不再手动创建
    // 直播时长使用 ui->label_liveDuration
    // 音频状态可以使用其他合适的位置或添加新标签

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
        if (ui->label_liveDuration) ui->label_liveDuration->setText(text);
    });

    // 统计信息更新定时器 (推流时每秒更新一次)
    stats_update_timer_ = new QTimer(this);
    connect(stats_update_timer_, &QTimer::timeout, [this]() {
        update_streaming_stats();
    });

    // 系统信息更新定时器 (每2秒更新一次)
    system_info_timer_ = new QTimer(this);
    connect(system_info_timer_, &QTimer::timeout, [this]() {
        update_system_info();
    });

    // 系统监控日志打印定时器 (每分钟打印一次)
    system_log_timer_ = new QTimer(this);
    connect(system_log_timer_, &QTimer::timeout, [this]() {
        log_system_stats_periodically();
    });
}

void MainWindow::setup_ui_connections() {
    // Settings dialog open (both gears)
    auto open_settings_dialog = [this](SettingsTab defaultTab = SettingsTab::Video) {
        if (!encoder_ || !audio_engine_) {
            QMessageBox::warning(this, "错误", "编码器或音频模块未初始化");
            return;
        }

        SettingsPanel dlg(this, defaultTab);
        
        // 设置视频配置
        dlg.set_video_config(encoder_->get_video_config());
        
        // 设置音频配置
        dlg.set_audio_config(encoder_->get_audio_config());
        
        // 设置麦克风列表
        dlg.set_available_microphones(audio_engine_->get_available_microphones(), 
                                       audio_engine_->get_selected_microphone_id());
        
        // 设置扬声器列表
        dlg.set_available_speakers(audio_engine_->get_available_speakers(),
                                    audio_engine_->get_selected_speaker_id());
        
        // 设置摄像头列表
        if (video_engine_) {
            dlg.set_available_cameras(video_engine_->get_available_camera_choices());
        }

        if (dlg.exec() == QDialog::Accepted) {
            applySettingsPanelChanges(dlg);
        }
    };

    // Settings dialog open (top bar settings button removed)
    if (ui->pushButton_settings) {
        connect(ui->pushButton_settings, &QPushButton::clicked, this, [this, open_settings_dialog]() {
            open_settings_dialog(SettingsTab::Video);
        });
    }

    // 状态标签初始化
    if (ui->label_status) {
        ui->label_status->setText("预览中");
        ui->label_status->setObjectName("labelPreviewStatus");
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
                    case AddMaterialDialog::Selection::Video:
                        // 直接打开插播视频列表对话框，与插播视频按钮逻辑一致
                        show_insert_video_widget();
                        break;
                    default:
                        QMessageBox::information(this, "提示", "功能开发中...");
                        break;
                }
            }
        });
    }

    if (ui->pushButton_startLive) {
        ui->pushButton_startLive->setObjectName("btnStartLive");
        connect(ui->pushButton_startLive, &QPushButton::clicked, this, [this]() {
            if (!encoder_bridge_) {
                QMessageBox::warning(this, "错误", "推流系统未初始化");
                return;
            }

            if (encoder_bridge_->is_streaming()) {
                // 停止推流 - 显示结束直播确认对话框
                QMessageBox::StandardButton reply = QMessageBox::question(
                    this,
                    "结束直播",
                    "确认结束直播么？",
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No  // 默认选择"否"
                );

                if (reply == QMessageBox::Yes) {
                    // 用户确认结束直播
                    encoder_bridge_->stop_streaming();
                    ui->pushButton_startLive->setText("开始直播");
                    if (ui->label_status) {
                        ui->label_status->setText("推流结束");
                    }
                    // 停止计时器
                    if (live_duration_timer_) {
                        live_duration_timer_->stop();
                        streaming_start_time_ms_ = 0;
                        if (ui->label_liveDuration) {
                            ui->label_liveDuration->setText("00:00:00");
                        }
                    }
                    // 停止统计信息更新定时器
                    if (stats_update_timer_) {
                        stats_update_timer_->stop();
                    }
                    if (system_info_timer_) {
                        system_info_timer_->stop();
                    }
                    LOG_INFO("直播已结束");
                }
                // 如果用户选择"否"，什么都不做
                return;
            }

            // 开始推流 - 显示开始直播确认对话框
            LOG_INFO("[DIAG] 准备开始推流");
            QMessageBox::StandardButton reply = QMessageBox::question(
                this,
                "开始直播",
                "确认开始直播么？",
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No  // 默认选择"否"
            );

            if (reply == QMessageBox::Yes) {
                // 用户确认开始直播
                QString url = rtmp_server_url_;
                if (url.isEmpty()) {
                    // 如果没有预设的推流地址，使用默认地址
                    url = "rtmp://47.92.156.37:1935/live/aaa";
                }

                if (encoder_bridge_->start_streaming(url.toStdString())) {
                    ui->pushButton_startLive->setText("停止直播");
                    if (ui->label_status) {
                        ui->label_status->setText("正在推流");
                    }
                    // 启动直播时长计时器
                    streaming_start_time_ms_ = QDateTime::currentMSecsSinceEpoch();
                    if (live_duration_timer_) {
                        live_duration_timer_->start(1000);
                    }
                    // 启动统计信息更新定时器
                    if (stats_update_timer_) {
                        stats_update_timer_->start(1000);
                    }
                    // 启动系统信息更新定时器
                    if (system_info_timer_) {
                        system_info_timer_->start(2000);
                    }
                    LOG_INFO("推流已启动: " + url.toStdString());
                } else {
                    QMessageBox::warning(this, "错误", "开始推流失败，请检查推流地址");
                }
            }
            // 如果用户选择"否"，什么都不做
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

    // 插播视频按钮连接
    if (ui->pushButton_insertVideo) {
        connect(ui->pushButton_insertVideo, &QPushButton::clicked, this, &MainWindow::on_insert_video_button_clicked);
    }
}

void MainWindow::setupBottomButtonsStyle() {
    // 设置底部控制栏按钮样式 - 与开始直播按钮风格统一但颜色区分
    // 开始直播: #4a6ef0 -> #f05a6a (紫红)
    
    // 共享屏幕: #2196F3 -> #00BCD4 (青色)
    QString shareScreenStyle = R"(
        QPushButton {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, 
                                        stop:0 #2196F3, stop:1 #00BCD4);
            color: white;
            border-radius: 6px;
            padding: 6px 12px;
            font-size: 13px;
            font-weight: bold;
            min-width: 80px;
            min-height: 28px;
        }
        QPushButton:hover {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, 
                                        stop:0 #42A5F5, stop:1 #26C6DA);
        }
        QPushButton:pressed {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, 
                                        stop:0 #1976D2, stop:1 #0097A7);
        }
    )";

    // 摄像头: #4CAF50 -> #8BC34A (绿色)
    QString cameraStyle = R"(
        QPushButton {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, 
                                        stop:0 #4CAF50, stop:1 #8BC34A);
            color: white;
            border-radius: 6px;
            padding: 6px 12px;
            font-size: 13px;
            font-weight: bold;
            min-width: 80px;
            min-height: 28px;
        }
        QPushButton:hover {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, 
                                        stop:0 #66BB6A, stop:1 #9CCC65);
        }
        QPushButton:pressed {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, 
                                        stop:0 #388E3C, stop:1 #689F38);
        }
    )";

    // 插播视频: #FF9800 -> #FF5722 (橙红色)
    QString insertVideoStyle = R"(
        QPushButton {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, 
                                        stop:0 #FF9800, stop:1 #FF5722);
            color: white;
            border-radius: 6px;
            padding: 6px 12px;
            font-size: 13px;
            font-weight: bold;
            min-width: 80px;
            min-height: 28px;
        }
        QPushButton:hover {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, 
                                        stop:0 #FFA726, stop:1 #FF7043);
        }
        QPushButton:pressed {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, 
                                        stop:0 #F57C00, stop:1 #E64A19);
        }
    )";

    // 应用不同的样式并连接点击事件
    if (ui->pushButton_shareScreen) {
        ui->pushButton_shareScreen->setStyleSheet(shareScreenStyle);
        // 连接共享屏幕按钮点击事件
        connect(ui->pushButton_shareScreen, &QPushButton::clicked, this, [this]() {
            LOG_INFO("Share screen button clicked from bottom toolbar");
            show_screen_share_selector();
        });
    }
    if (ui->pushButton_camera) {
        ui->pushButton_camera->setStyleSheet(cameraStyle);
        // 连接摄像头按钮点击事件
        connect(ui->pushButton_camera, &QPushButton::clicked, this, [this]() {
            LOG_INFO("Camera button clicked from bottom toolbar");
            on_camera_button_clicked();
        });
    }
    if (ui->pushButton_insertVideo) {
        ui->pushButton_insertVideo->setStyleSheet(insertVideoStyle);
    }
    
    LOG_INFO("Bottom buttons styles applied with different colors");
}

void MainWindow::on_insert_video_button_clicked() {
    LOG_INFO("Insert video button clicked");
    show_insert_video_widget();
}

void MainWindow::show_insert_video_widget() {
    if (!insert_video_widget_) {
        insert_video_widget_ = new InsertVideoWidget(this);

        // 设置直播间信息
        // 从 current_live_item_ 获取直播间ID
        QString roomId = current_live_item_.liveId;
        if (!roomId.isEmpty()) {
            // 注意：需要传递 live_url_，不能传空字符串
            insert_video_widget_->setLiveInfo(live_url_, user_id_, token_, roomId);
        }

        // 连接开始插播信号
        connect(insert_video_widget_, &InsertVideoWidget::startInsertVideo,
                this, &MainWindow::on_start_insert_video);
    }

    insert_video_widget_->show();
    insert_video_widget_->raise();
    insert_video_widget_->activateWindow();
}

void MainWindow::on_start_insert_video(const QString& fileId, const QString& fileName, bool loopEnabled) {
    LOG_INFO("Starting insert video: " + fileId.toStdString() + " - " + fileName.toStdString() +
             ", loopEnabled=" + std::to_string(loopEnabled));

    // 如果已经有插播视频在播放，先停止
    if (is_insert_video_playing_) {
        stopInsertVideoPlayback();
    }

    startInsertVideoPlayback(fileId, loopEnabled);
}

void MainWindow::startInsertVideoPlayback(const QString& fileId, bool loopEnabled) {
    auto fileItem = InsertFileManager::instance()->getFile(fileId);
    if (!fileItem) {
        LOG_ERROR("Insert video file not found: " + fileId.toStdString());
        QMessageBox::warning(this, "错误", "插播视频文件未找到");
        return;
    }

    if (!fileItem->isDownloaded()) {
        LOG_ERROR("Insert video file not downloaded: " + fileId.toStdString());
        QMessageBox::warning(this, "错误", "插播视频文件尚未下载完成");
        return;
    }

    // 创建 MediaFileSource
    std::string source_id = "insert_video_" + fileId.toStdString();
    auto mediaSource = std::make_shared<MediaFileSource>(source_id, fileItem);

    // 设置循环播放
    mediaSource->set_loop_enabled(loopEnabled);

    if (!mediaSource->initialize()) {
        LOG_ERROR("Failed to initialize media file source");
        QMessageBox::warning(this, "错误", "初始化插播视频源失败");
        return;
    }

    // 添加到场景
    if (!scene_manager_) {
        LOG_ERROR("Scene manager not initialized");
        return;
    }

    auto scene = scene_manager_->get_current_scene();
    if (!scene) {
        LOG_ERROR("No current scene");
        return;
    }

    // 添加到场景（全屏显示）
    auto sceneItem = scene->add_source(mediaSource);
    if (sceneItem) {
        // 设置全屏变换
        Transform transform(0, 0, canvas_config_.get_width(), canvas_config_.get_height());
        scene->set_transform(sceneItem, transform);
        
        // 设置插播视频的order为最低（0），确保在摄像头之下渲染（先渲染的在下面）
        sceneItem->set_order(0);

        // 添加到 Compositor（用于推流渲染）
        if (compositor_) {
            const std::string source_id_str = mediaSource->get_id();
            if (!compositor_->has_layer(source_id_str)) {
                compositor_->add_layer(source_id_str);
                // 设置最低的 z_order，确保插播视频在底层（被摄像头覆盖）
                compositor_->set_layer_order(source_id_str, 0);
            }
            compositor_->update_layer_transform(source_id_str, 
                QRectF(0, 0, canvas_config_.get_width(), canvas_config_.get_height()), 1.0f);
        }

        // 设置帧回调 - 将解码后的 VideoFrame 传递给 Compositor
        // 零拷贝方案：传递 shared_ptr<VideoFrame>，数据生命周期由智能指针管理
        mediaSource->set_frame_ready_callback(
            [this, mediaSource](std::shared_ptr<VideoFrame> frame) {
                if (!frame || !frame->data) return;

                // 直接传递 shared_ptr<VideoFrame>，零拷贝
                const std::string source_id = mediaSource->get_id();
                if (compositor_) {
                    compositor_->update_layer_video_frame(source_id, frame);
                }
                // 注意：不再直接调用 repaint，由 CanvasWidget 的定时器驱动刷新
            }
        );

        // 启动播放
        if (mediaSource->start()) {
            current_insert_video_source_ = mediaSource;
            current_insert_video_file_id_ = fileId;
            is_insert_video_playing_ = true;

            // 设置混音模式：麦克风 + 插播音频
            if (audio_engine_) {
                audio_engine_->setMixMode(AudioMixMode::MIC_MEDIA);
                audio_engine_->set_media_volume(0.7f);  // 默认插播音量为70%

                // 注册插播音频源回调
                audio_engine_->registerAudioSourceCallback(
                    QString::fromStdString(source_id),
                    [mediaSource]() -> std::shared_ptr<AudioFrame> {
                        if (mediaSource && mediaSource->is_running()) {
                            return mediaSource->get_audio_frame();
                        }
                        return nullptr;
                    }
                );

                LOG_INFO("Audio mix mode set to MIC_MEDIA");
            }

            LOG_INFO("Insert video playback started: " + fileItem->fileName.toStdString());

            // 更新场景列表UI
            sync_scene_to_compositor();

            // 关键修复：确保所有摄像头源保持最高的 order 值，永远在最上层
            auto scene = scene_manager_->get_current_scene();
            if (scene) {
                auto items = scene->get_all_scene_items();
                for (auto& item : items) {
                    if (item && item->get_source()) {
                        auto src = item->get_source();
                        // 检查是否是摄像头源（VIDEO_CAPTURE 类型）
                        if (src->get_type() == Source::Type::VIDEO_CAPTURE) {
                            item->set_order(9999); // 保持摄像头的最高order值
                        }
                    }
                }
                scene->normalize_orders();
                // 重新同步到 compositor，确保 order 值正确应用
                sync_scene_to_compositor();
            }

            build_scene_list();

            // 连接播放完成回调
            // TODO: 实现播放完成检测
        } else {
            LOG_ERROR("Failed to start media source");
            scene->remove_scene_item(sceneItem);
            QMessageBox::warning(this, "错误", "启动插播视频失败");
        }
    }
}

void MainWindow::stopInsertVideoPlayback() {
    if (!is_insert_video_playing_) {
        return;
    }

    LOG_INFO("Stopping insert video playback");

    // 从场景中移除
    if (scene_manager_ && current_insert_video_source_) {
        auto scene = scene_manager_->get_current_scene();
        if (scene) {
            scene->remove_source(current_insert_video_source_->get_id());
        }
    }

    // 停止并清理源
    if (current_insert_video_source_) {
        current_insert_video_source_->stop();
        current_insert_video_source_->shutdown();
        current_insert_video_source_.reset();
    }

    current_insert_video_file_id_.clear();
    is_insert_video_playing_ = false;

    // 恢复混音模式：只用麦克风
    if (audio_engine_) {
        audio_engine_->setMixMode(AudioMixMode::MIC_ONLY);
        // 注销插播音频源回调
        if (!current_insert_video_file_id_.isEmpty()) {
            QString callbackId = QString("insert_video_%1").arg(current_insert_video_file_id_);
            audio_engine_->unregisterAudioSource(callbackId);
        }
        LOG_INFO("Audio mix mode restored to MIC_ONLY");
    }

    // 更新UI
    sync_scene_to_compositor();
    build_scene_list();

    LOG_INFO("Insert video playback stopped");
}

void MainWindow::on_stop_insert_video() {
    stopInsertVideoPlayback();
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
    dialog.set_available_cameras(camera_choices);
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
                        // First-frame sizing: if no transform set, fit to quarter canvas preserving aspect ratio
                        if (scene_manager_ && scene_manager_->get_current_scene()) {
                            auto scene = scene_manager_->get_current_scene();
                            auto items = scene->get_all_scene_items();
                            for (auto& it : items) {
                                if (it && it->get_source_id() == source_id) {
                                    Transform tr = it->get_transform();
                                    if (tr.width == 0 && tr.height == 0) {
                                        int canvas_w = canvas_widget_ ? canvas_widget_->width() : canvas_config_.get_width();
                                        int canvas_h = canvas_widget_ ? canvas_widget_->height() : canvas_config_.get_height();
                                        int max_w = canvas_w / 2;
                                        int max_h = canvas_h / 2;
                                        int src_w = frame.image.width();
                                        int src_h = frame.image.height();
                                        if (src_w > 0 && src_h > 0) {
                                            double sx = static_cast<double>(max_w) / src_w;
                                            double sy = static_cast<double>(max_h) / src_h;
                                            double s = (sx < sy ? sx : sy);
                                            int target_w = static_cast<int>(src_w * s);
                                            int target_h = static_cast<int>(src_h * s);
                                            int x = (canvas_w - target_w) / 2;
                                            int y = (canvas_h - target_h) / 2;
                                            Transform newTr(x, y, target_w, target_h, 0.0f, 1.0f);
                                            scene->set_transform(it, newTr);
                                            update_scene_items();
                                            if (canvas_widget_) canvas_widget_->refresh();
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                        break;
                    }

                    // 尝试更新 CameraSource（摄像头）
                    auto cameraSrc = std::dynamic_pointer_cast<CameraSource>(item->get_source());
                    if (cameraSrc) {
                        LOG_INFO("[DIAG] 更新CameraSource图像: " + source_id);
                        cameraSrc->push_frame(frame.image);
                        // If this is the first frame and the scene item has no size, fit it to 1/4 canvas preserving aspect ratio
                        if (scene_manager_ && scene_manager_->get_current_scene()) {
                            auto scene = scene_manager_->get_current_scene();
                            auto items = scene->get_all_scene_items();
                            for (auto& it : items) {
                                if (it && it->get_source_id() == source_id) {
                                    Transform tr = it->get_transform();
                                    if (tr.width == 0 && tr.height == 0) {
                                        int canvas_w = canvas_widget_ ? canvas_widget_->width() : canvas_config_.get_width();
                                        int canvas_h = canvas_widget_ ? canvas_widget_->height() : canvas_config_.get_height();
                                        int max_w = canvas_w / 2;
                                        int max_h = canvas_h / 2;
                                        int src_w = frame.image.width();
                                        int src_h = frame.image.height();
                                        if (src_w > 0 && src_h > 0) {
                                            double sx = static_cast<double>(max_w) / src_w;
                                            double sy = static_cast<double>(max_h) / src_h;
                                            double s = (sx < sy ? sx : sy);
                                            int target_w = static_cast<int>(src_w * s);
                                            int target_h = static_cast<int>(src_h * s);
                                            int x = (canvas_w - target_w) / 2;
                                            int y = (canvas_h - target_h) / 2;
                                            Transform newTr(x, y, target_w, target_h, 0.0f, 1.0f);
                                            scene->set_transform(it, newTr);
                                            update_scene_items();
                                            if (canvas_widget_) canvas_widget_->refresh();
                                        }
                                    }
                                    break;
                                }
                            }
                        }
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
            auto added_item = scene->add_source(camera_source);
            // If we have a canvas size available, set the scene item's transform to fill the canvas
            if (added_item) {
                // 使用canvas_config_的逻辑尺寸，确保视频源初始尺寸合理
                int canvas_w = canvas_config_.get_width(); // 默认1920
                int canvas_h = canvas_config_.get_height(); // 默认1080
                int w = canvas_w / 2;
                int h = canvas_h / 2;
                int x = (canvas_w - w) / 2;
                int y = (canvas_h - h) / 2;
                Transform tr(x, y, w, h, 0.0f, 1.0f);
                scene->set_transform(added_item, tr);
                
                // 设置摄像头的order为最高，确保它永远在最上层
                added_item->set_order(9999); // 设置一个很高的值
                scene->normalize_orders();
            }
            LOG_INFO("更新场景项并同步到Compositor");
            update_scene_items();  // 确保compositor中有对应的图层
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
                        auto added_item = scene->add_source(screen_src);
                        if (added_item) {
                            // 使用canvas_config_的逻辑尺寸，让视频源自适应满画布
                            int canvas_w = canvas_config_.get_width(); // 默认1920
                            int canvas_h = canvas_config_.get_height(); // 默认1080
                            
                            // 假设视频源的原始宽高比（这里使用16:9作为默认值，实际应该从视频源获取）
                            // 注意：实际应用中应该从视频源获取真实的宽高比
                            int src_w = 1920; // 假设视频源宽度
                            int src_h = 1080; // 假设视频源高度
                            
                            // 计算缩放比例，取较小值以保证完全显示
                            double scale_w = static_cast<double>(canvas_w) / src_w;
                            double scale_h = static_cast<double>(canvas_h) / src_h;
                            double scale = (std::min)(scale_w, scale_h);
                            
                            // 计算缩放后的尺寸
                            int target_w = static_cast<int>(src_w * scale);
                            int target_h = static_cast<int>(src_h * scale);
                            
                            // 计算居中位置
                            int x = (canvas_w - target_w) / 2;
                            int y = (canvas_h - target_h) / 2;
                            
                            Transform tr(x, y, target_w, target_h, 0.0f, 1.0f);
                            scene->set_transform(added_item, tr);
                        }
                        
                        // 确保所有摄像头项仍然保持最高的order值
                        auto items = scene->get_all_scene_items();
                        for (auto& item : items) {
                            if (QString::fromStdString(item->get_source_id()).startsWith("camera_")) {
                                item->set_order(9999); // 保持摄像头的最高order值
                            }
                        }
                        scene->normalize_orders();
                        
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

    // 只记录日志，不弹窗提示
    if (capture_manager_ && capture_manager_->has_source(source_id)) {
        LOG_INFO("Screen sharing already active for: " + target_id.toStdString());
    } else {
        LOG_INFO("Screen share target selected, WGC capture started for: " + target_id.toStdString());
    }
}

void MainWindow::on_camera_frame_ready() {
}

void MainWindow::update_status(const QString& message) {
    // 状态栏已移除
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

    // To avoid crashes from dangling event filters or transient stageContainer_, insert the canvas
    // directly into the layout so it is managed by the UI layout system (stable and predictable).
    if (ui->label_livePreview) {
        ui->verticalLayout_liveArea->removeWidget(ui->label_livePreview);
        delete ui->label_livePreview;
        ui->label_livePreview = nullptr;
    }

    // If a stageContainer_ exists, remove it and its placeholder to avoid conflicting ownership.
    if (stageContainer_) {
        // remove event filters safely
        stageContainer_->removeEventFilter(this);
        if (stagePlaceholderWidget_) stagePlaceholderWidget_->removeEventFilter(this);
        // detach widgets
        stageContainer_->setParent(nullptr);
        if (stagePlaceholderWidget_) {
            stagePlaceholderWidget_->setParent(nullptr);
            delete stagePlaceholderWidget_;
            stagePlaceholderWidget_ = nullptr;
        }
        delete stageContainer_;
        stageContainer_ = nullptr;
        placeholderIcon_ = nullptr;
        placeholderText_ = nullptr;
        if (stageAddButton_) { delete stageAddButton_; stageAddButton_ = nullptr; }
    }

    canvas_widget_->setParent(ui->centralWidget);
    canvas_widget_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    ui->verticalLayout_liveArea->insertWidget(0, canvas_widget_);
    canvas_widget_->installEventFilter(this); // Install event filter to handle resize events
    canvas_widget_->show();
    // create placeholder overlays as children of the canvas so they stay on top and move/resize with it
    // Note: placeholderIcon_ (✚) is kept for backward compatibility but hidden, only + button is shown
    if (!placeholderIcon_) {
        placeholderIcon_ = new QLabel(canvas_widget_);
        placeholderIcon_->setText(QString::fromUtf8("✚"));
        QFont iconFont = placeholderIcon_->font();
        iconFont.setPointSize(48);
        placeholderIcon_->setFont(iconFont);
        placeholderIcon_->setAlignment(Qt::AlignCenter);
        placeholderIcon_->setStyleSheet("color: rgba(255,255,255,0.18); background: transparent;");
        placeholderIcon_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        placeholderIcon_->hide(); // Hidden by default, only + button is shown
    }
    if (!placeholderText_) {
        placeholderText_ = new QLabel(QString::fromUtf8("添加直播画面"), canvas_widget_);
        QFont txtF = placeholderText_->font();
        txtF.setPointSize(16);
        placeholderText_->setFont(txtF);
        placeholderText_->setAlignment(Qt::AlignCenter);
        placeholderText_->setStyleSheet("color: #BFBFBF; background: transparent;");
        placeholderText_->setAttribute(Qt::WA_TransparentForMouseEvents, true); // Label is not clickable
        placeholderText_->show();
    }
    if (!stageAddButton_) {
        stageAddButton_ = new QPushButton(QString::fromUtf8("+"), canvas_widget_);
        stageAddButton_->setCursor(Qt::PointingHandCursor);
        stageAddButton_->setFixedSize(140, 44);
        stageAddButton_->setStyleSheet(
            "QPushButton { background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #444, stop:1 #333); color: white; border-radius: 6px; font-size: 14px; }"
            "QPushButton:hover { background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #555, stop:1 #444); }"
            "QPushButton:pressed { background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #333, stop:1 #222); }"
        );
        // forward to the right-side add material logic so behavior is identical
        if (ui->pushButton_addMaterial) {
            connect(stageAddButton_, &QPushButton::clicked, this, [this]() {
                ui->pushButton_addMaterial->click();
            });
        } else {
            connect(stageAddButton_, &QPushButton::clicked, this, [this]() {
                AddMaterialDialog dlg(this);
                dlg.exec();
            });
        }
        stageAddButton_->show();
    }
    // position overlays: + button above "添加直播画面" text
    repositionPlaceholderOverlays();

    connect(canvas_widget_, &CanvasWidget::scene_item_selected, this, [this](std::shared_ptr<SceneItem> item) {
        LOG_INFO("Scene item selected: " + (item ? item->get_source_id() : "null"));
    });

    connect(canvas_widget_, &CanvasWidget::scene_item_moved, this,
        [this](std::shared_ptr<SceneItem>, const Transform&, const Transform&) {
            sync_scene_to_compositor();
        });

    LOG_INFO("Canvas widget setup completed");
    // After canvas inserted, update placeholder visibility
    updateStagePlaceholderVisibility();
}

void MainWindow::update_preview() {
    if (canvas_widget_) {
        canvas_widget_->refresh();
    }

    // Camera frames are now driven by CaptureManager camera sources.
}


void MainWindow::update_streaming_stats() {
    if (!stream_pusher_ || !stream_pusher_->is_pushing()) {
        return;
    }

    // 注意：新的 RTMPPusherNew 使用信号驱动的统计更新（statisticsUpdated signal）
    // 这里保留占位以防需要手动触发

    // 使用新的 SystemMonitor 获取系统信息
    const auto& sys_stats = system_monitor().get_cached_stats();
    QString memory_text;
    if (sys_stats.gpu_available) {
        memory_text = QString("内存: %1GB/%2GB (%3%) | GPU: %4% (%5GB/%6GB)")
            .arg(sys_stats.memory_used_bytes / 1024.0 / 1024.0 / 1024.0, 0, 'f', 1)
            .arg(sys_stats.memory_total_bytes / 1024.0 / 1024.0 / 1024.0, 0, 'f', 1)
            .arg(sys_stats.memory_usage_percent, 0, 'f', 0)
            .arg(sys_stats.gpu_usage_percent, 0, 'f', 0)
            .arg(sys_stats.gpu_memory_used_bytes / 1024.0 / 1024.0 / 1024.0, 0, 'f', 1)
            .arg(sys_stats.gpu_memory_total_bytes / 1024.0 / 1024.0 / 1024.0, 0, 'f', 1);
    } else {
        memory_text = QString("内存: %1GB/%2GB (%3%)")
            .arg(sys_stats.memory_used_bytes / 1024.0 / 1024.0 / 1024.0, 0, 'f', 1)
            .arg(sys_stats.memory_total_bytes / 1024.0 / 1024.0 / 1024.0, 0, 'f', 1)
            .arg(sys_stats.memory_usage_percent, 0, 'f', 0);
    }

    // 注意：新的 RTMPPusherNew 使用信号驱动的统计更新
    // 这里显示简化状态（完整的统计由 statisticsUpdated 信号处理）
    QString status_text = QString("状态: 推流中 | CPU: %1% | %2")
        .arg(QString::number(sys_stats.cpu_usage_percent, 'f', 1))
        .arg(memory_text);

    if (ui->label_techStats) {
        ui->label_techStats->setText(status_text);
        ui->label_techStats->setStyleSheet("font-size: 12px; color: #cccccc;");
    }
}

void MainWindow::repositionPlaceholderOverlays() {
    if (!canvas_widget_ || !placeholderText_ || !stageAddButton_) return;
    
    QRect canvasRect = canvas_widget_->rect();
    int centerX = canvasRect.center().x();
    int centerY = canvasRect.center().y();
    
    // Calculate text size
    QFontMetrics fm(placeholderText_->font());
    QRect textRect = fm.boundingRect(placeholderText_->text());
    int textWidth = textRect.width();
    int textHeight = textRect.height();
    
    // Button size
    int buttonWidth = stageAddButton_->width();
    int buttonHeight = stageAddButton_->height();
    
    // Spacing between button and text (20px)
    int spacing = 20;
    
    // Position text at center
    int textX = centerX - textWidth / 2;
    int textY = centerY + textHeight / 2;
    
    // Position button above text
    int buttonX = centerX - buttonWidth / 2;
    int buttonY = centerY - textHeight / 2 - spacing - buttonHeight;
    
    // Set geometries
    placeholderText_->setGeometry(textX, textY - textHeight, textWidth, textHeight);
    stageAddButton_->setGeometry(buttonX, buttonY, buttonWidth, buttonHeight);
}

void MainWindow::updateStagePlaceholderVisibility() {
    bool hasVideoSource = false;
    if (scene_manager_ && scene_manager_->get_current_scene()) {
        auto items = scene_manager_->get_current_scene()->get_all_scene_items();
        hasVideoSource = !items.empty();
    }
    // do not treat presence of canvas_widget_ alone as content; rely on scene items
    if (!canvas_widget_ || !placeholderText_ || !stageAddButton_) return;

    if (hasVideoSource) {
        if (placeholderIcon_) placeholderIcon_->hide();
        placeholderText_->hide();
        stageAddButton_->hide();
    } else {
        // position overlays relative to canvas
        repositionPlaceholderOverlays();
        if (placeholderIcon_) placeholderIcon_->hide(); // Hide the ✚ icon, only show + button
        placeholderText_->show();
        stageAddButton_->show();
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == stageContainer_) {
        if (event->type() == QEvent::Resize) {
            if (stageAddButton_ && stageContainer_) {
                stageAddButton_->setGeometry(stageContainer_->rect());
            }
            if (canvas_widget_ && stageContainer_) {
                canvas_widget_->setGeometry(stageContainer_->rect());
            }
            // reposition stage control buttons
            if (stageBtnMax_ && stageBtnMin_ && stageBtnRestore_ && stageContainer_) {
                stageBtnMax_->move(stageContainer_->width() - 26, 6);
                stageBtnMin_->move(stageContainer_->width() - 52, 6);
                stageBtnRestore_->move(stageContainer_->width() - 78, 6);
                stageBtnMax_->raise(); stageBtnMin_->raise(); stageBtnRestore_->raise();
            }
            return false;
        }

        // no per-stage dragging (stage fills placeholder and is controlled by main window)
    }
    // handle window dragging via topBar (move the whole main window)
    if (ui && watched == ui->topBar) {
        if (event->type() == QEvent::MouseButtonPress) {
            QMouseEvent* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                // Check if click is on a button (don't drag if clicking buttons)
                QWidget* child = ui->topBar->childAt(me->pos());
                if (child && (qobject_cast<QPushButton*>(child) || child->parent() == ui->topBar)) {
                    // Check if it's one of the window control buttons (top bar settings button removed)
                    if (child == ui->pushButton_minimize || 
                        child == ui->pushButton_maximize || 
                        child == ui->pushButton_close) {
                        return false; // Let the button handle the click
                    }
                }
                window_dragging_ = true;
                window_drag_start_pos_ = me->globalPos() - this->pos();
                return true;
            }
        }
        if (event->type() == QEvent::MouseMove) {
            QMouseEvent* me = static_cast<QMouseEvent*>(event);
            if (window_dragging_ && (me->buttons() & Qt::LeftButton)) {
                QPoint gp = me->globalPos();
                this->move(gp - window_drag_start_pos_);
                return true;
            }
        }
        if (event->type() == QEvent::MouseButtonRelease) {
            QMouseEvent* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton && window_dragging_) {
                window_dragging_ = false;
                return true;
            }
        }
    }
    if (ui && watched == ui->liveArea && event->type() == QEvent::Resize) {
        if (stageContainer_ && stagePlaceholderWidget_) {
            // match the placeholder's geometry so stage fills the available area
            QRect phGeom = stagePlaceholderWidget_->geometry();
            stageContainer_->setGeometry(0, 0, phGeom.width(), phGeom.height());
            if (canvas_widget_) canvas_widget_->setGeometry(stageContainer_->rect());
        }
        // Reposition placeholder overlays when liveArea resizes
        repositionPlaceholderOverlays();
        return false;
    }
    // Handle canvas widget resize to reposition placeholder overlays
    if (watched == canvas_widget_ && event->type() == QEvent::Resize) {
        repositionPlaceholderOverlays();
        return false;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::update_system_info() {
    // 使用新的 SystemMonitor 模块获取系统信息
    system_monitor().update();
}

void MainWindow::toggleStageMaximize() {
    if (!stageContainer_ || !ui || !ui->liveArea) return;
    if (!stage_maximized_) {
        // if minimized, restore first
        if (stage_minimized_ && stageRestoreButton_) {
            stageRestoreButton_->click();
        }
        stage_normal_geometry_ = stageContainer_->geometry();
        QRect targetLocal = ui->liveArea->rect();
        targetLocal.adjust(6, 6, -6, -6);
        if (targetLocal.width() < 100) targetLocal.setWidth(qMax(100, ui->liveArea->width() - 12));
        if (targetLocal.height() < 60) targetLocal.setHeight(qMax(60, ui->liveArea->height() - 12));
        stageContainer_->setGeometry(targetLocal);
        stage_maximized_ = true;
        stageBtnRestore_->setVisible(true);
        if (stageBtnMax_) { stageBtnMax_->setText("⧉"); stageBtnMax_->setToolTip("还原"); }
        if (ui->pushButton_maximize) { ui->pushButton_maximize->setText("⧉"); ui->pushButton_maximize->setToolTip("还原"); }
    } else {
        // restore
        stageContainer_->setGeometry(stage_normal_geometry_);
        stage_maximized_ = false;
        stageBtnRestore_->setVisible(false);
        if (stageBtnMax_) { stageBtnMax_->setText("□"); stageBtnMax_->setToolTip("最大化"); }
        if (ui->pushButton_maximize) { ui->pushButton_maximize->setText("□"); ui->pushButton_maximize->setToolTip("最大化"); }
    }
}

void MainWindow::restoreStage() {
    if (!stageContainer_) return;
    stageContainer_->setGeometry(stage_normal_geometry_);
    stage_maximized_ = false;
    stageBtnRestore_->setVisible(false);
    if (stageBtnMax_) { stageBtnMax_->setText("□"); stageBtnMax_->setToolTip("最大化"); }
    if (ui->pushButton_maximize) { ui->pushButton_maximize->setText("□"); ui->pushButton_maximize->setToolTip("最大化"); }
}
// 新增：定期打印系统统计日志（每分钟打印一次）
void MainWindow::log_system_stats_periodically() {
    const auto& stats = system_monitor().get_cached_stats();

    system_log_counter_++;
    LOG_INFO("=== 系统资源监控 (" + std::to_string(system_log_counter_) + ") ===");
    LOG_INFO("CPU 使用率: " + std::to_string(stats.cpu_usage_percent) + "%");
    LOG_INFO("内存: " + std::to_string(stats.memory_used_bytes / 1024.0 / 1024.0 / 1024.0) +
             "GB / " + std::to_string(stats.memory_total_bytes / 1024.0 / 1024.0 / 1024.0) +
             "GB (" + std::to_string(stats.memory_usage_percent) + "%)");

    if (stats.gpu_available) {
        LOG_INFO("GPU 使用率: " + std::to_string(stats.gpu_usage_percent) + "%");
        LOG_INFO("GPU 内存: " + std::to_string(stats.gpu_memory_used_bytes / 1024.0 / 1024.0 / 1024.0) +
                 "GB / " + std::to_string(stats.gpu_memory_total_bytes / 1024.0 / 1024.0 / 1024.0) +
                 "GB (" + std::to_string(stats.gpu_memory_usage_percent) + "%)");
    } else {
        LOG_INFO("GPU: N/A (无法获取 GPU 信息)");
    }
    LOG_INFO("====================================");
}

void MainWindow::encode_and_push() {
    // NOTE: Audio encoding is now handled by CompositorEncoderBridge
    // This timer callback is kept for future use if needed
    // Audio encoding was removed to avoid double-encoding with CompositorEncoderBridge
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

void MainWindow::show_scene_item_settings(int index) {
    if (!scene_manager_ || !scene_manager_->get_current_scene()) {
        LOG_ERROR("Scene manager or current scene not initialized");
        return;
    }
    
    auto scene = scene_manager_->get_current_scene();
    auto items = scene->get_all_scene_items();
    if (index < 0 || index >= static_cast<int>(items.size())) {
        return;
    }
    
    auto item = items[index];
    auto source = item->get_source();
    if (!source) {
        QMessageBox::information(this, "提示", "设置功能开发中...");
        return;
    }
    
    std::string source_id = source->get_id();
    QString source_type = QString::fromStdString(source->get_metadata());
    
    // 检查是否是摄像头源
    if (QString::fromStdString(source_id).startsWith("camera_")) {
        // 打开设置面板并切换到摄像头页面
        if (!encoder_ || !audio_engine_ || !video_engine_) {
            QMessageBox::warning(this, "错误", "模块未初始化");
            return;
        }

        SettingsPanel dlg(this, SettingsTab::Camera);
        
        // 设置视频配置
        dlg.set_video_config(encoder_->get_video_config());
        
        // 设置音频配置
        dlg.set_audio_config(encoder_->get_audio_config());
        
        // 设置麦克风列表
        dlg.set_available_microphones(audio_engine_->get_available_microphones(), 
                                       audio_engine_->get_selected_microphone_id());
        
        // 设置扬声器列表
        dlg.set_available_speakers(audio_engine_->get_available_speakers(),
                                    audio_engine_->get_selected_speaker_id());
        
        // 设置摄像头列表并选中当前摄像头
        auto camera_choices = video_engine_->get_available_camera_choices();
        dlg.set_available_cameras(camera_choices);
        
        // 获取当前摄像头配置
        // 这里可以从source中获取当前的分辨率、帧率等配置
        // 暂时使用默认值
        dlg.set_camera_config(source_id, "1280x720", 30, false);
        
        if (dlg.exec() == QDialog::Accepted) {
            // 应用通用设置
            applySettingsPanelChanges(dlg);
            
            // 摄像头特定处理
            const std::string camera_id = dlg.get_selected_camera_id();
            
            // 如果摄像头改变了，需要重新初始化摄像头
            if (camera_id != source_id) {
                // 提示用户需要重新添加摄像头
                QMessageBox::information(this, "提示", 
                    "摄像头已更改，需要重新添加摄像头。\n请删除当前摄像头后重新添加。");
            }
            
            LOG_INFO("Camera settings updated for source: " + source_id);
        }
    } else {
        // 非摄像头项，暂时显示开发中
        QMessageBox::information(this, "提示", "设置功能开发中...");
    }
}

void MainWindow::applySettingsPanelChanges(SettingsPanel& dlg) {
    // Apply video settings
    auto new_v = dlg.get_video_config();
    
    // Apply audio settings
    auto new_a = dlg.get_audio_config();
    new_a.bitrate = encoder_->get_audio_config().bitrate; // Keep bitrate from existing config
    
    const std::string mic_id = dlg.get_selected_microphone_id();
    const std::string speaker_id = dlg.get_selected_speaker_id();
    float mic_volume = dlg.get_microphone_volume();
    float speaker_volume = dlg.get_speaker_volume();

    // Reinit audio engine/encoder
    audio_engine_->initialize(new_a.sample_rate, new_a.channels);
    audio_engine_->set_microphone_volume(mic_volume);
    audio_engine_->set_speaker_volume(speaker_volume);
    encoder_->reinitialize_audio_encoder(new_a);

    // Reinit video encoder + bridge settings
    encoder_->reinitialize_video_encoder(new_v);
    if (encoder_bridge_) {
        encoder_bridge_->set_resolution(new_v.width, new_v.height);
        encoder_bridge_->set_fps(new_v.fps);
    }

    // Select microphone
    if (!mic_id.empty()) {
        audio_engine_->select_microphone(mic_id);
    }
    
    // Select speaker
    if (!speaker_id.empty()) {
        audio_engine_->select_speaker(speaker_id);
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

    // 注意：video_engine_->set_current_scene() 现在只在开始插播时调用一次
    // 不在这里调用，避免频繁同步造成性能问题

    auto items = scene->get_all_scene_items();
    
    LOG_INFO("[DIAG] sync_scene_to_compositor: scene=" + scene->get_name() + 
             ", items_count=" + std::to_string(items.size()));

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
    // 不再使用手动创建的标签显示音频状态
    // 可以在日志中记录音频状态
    LOG_INFO("Audio status: " + text.toStdString() + " (color: " + color.toStdString() + ")");
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
        ui->pushButton_mic->setText("");
        ui->pushButton_mic->setStyleSheet(microphone_enabled_ ?
            "border: none; background: transparent;" :
            "border: none; background: transparent; opacity: 0.5;");
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
                    update_audio_status("故障", "red");
                } else {
                    update_audio_status("正常", "green");
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
        ui->pushButton_speaker->setText("");
        ui->pushButton_speaker->setStyleSheet(speaker_enabled_ ?
            "border: none; background: transparent;" :
            "border: none; background: transparent; opacity: 0.5;");
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
    if (ui->label_liveDuration) ui->label_liveDuration->setText("00:00:00");
    if (live_duration_timer_) live_duration_timer_->start(1000);

    // 启动统计信息更新定时器
    if (stats_update_timer_) {
        stats_update_timer_->start(1000);
    }

    // 启动系统信息更新定时器
    if (system_info_timer_) {
        system_info_timer_->start(2000); // 每2秒更新一次
        // 立即更新一次系统信息
        update_system_info();
        // 立即更新一次统计信息
        update_streaming_stats();
    }

    // 启动系统监控日志打印定时器 (每分钟打印一次)
    if (system_log_timer_) {
        system_log_timer_->start(60000); // 60000ms = 1分钟
        system_log_counter_ = 0;
        // 立即打印一次初始状态
        log_system_stats_periodically();
    }
}

void MainWindow::on_streaming_stopped() {
    LOG_INFO("推流状态：已停止");
    if (live_duration_timer_) live_duration_timer_->stop();
    if (stats_update_timer_) stats_update_timer_->stop();
    if (system_info_timer_) system_info_timer_->stop();
    if (system_log_timer_) system_log_timer_->stop(); // 停止日志打印定时器
    streaming_start_time_ms_ = 0;
    if (ui->label_liveDuration) ui->label_liveDuration->setText("00:00:00");

    // 恢复默认状态显示
    if (ui->label_techStats) {
        ui->label_techStats->setText("码率: 0kb/s | FPS: 0.00 | CPU: 0.0% | 内存: 0.0MB");
        ui->label_techStats->setStyleSheet("font-size: 12px; color: #666666;");
    }
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

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    
    if (event->type() == QEvent::WindowStateChange) {
        if (ui->pushButton_maximize) {
            if (isMaximized()) {
                ui->pushButton_maximize->setText("◱");
                ui->pushButton_maximize->setProperty("maximized", true);
            } else {
                ui->pushButton_maximize->setText("⤡");
                ui->pushButton_maximize->setProperty("maximized", false);
            }
            ui->pushButton_maximize->style()->unpolish(ui->pushButton_maximize);
            ui->pushButton_maximize->style()->polish(ui->pushButton_maximize);
        }
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    LOG_INFO("MainWindow close event triggered");
    
    // 如果已经在退出过程中，直接接受
    if (is_exiting_) {
        event->accept();
        return;
    }
    
    // 如果正在推流，提示用户
    if (encoder_bridge_ && encoder_bridge_->is_streaming()) {
        int ret = QMessageBox::question(this, "推流进行中",
            "当前正在推流直播中，确定要退出吗？\n退出后将中断直播推流。",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret == QMessageBox::No) {
            event->ignore();
            return;
        }
    }
    
    // 显示退出确认对话框
    ExitDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        ExitDialog::Action action = dialog.getSelectedAction();
        
        // 如果用户选择了记住选择
        if (dialog.shouldRememberChoice()) {
            if (action == ExitDialog::Action::Minimize) {
                saveExitPreference(1); // 记住最小化
            } else {
                saveExitPreference(2); // 记住退出
            }
        }
        
        if (action == ExitDialog::Action::Minimize) {
            LOG_INFO("User chose to minimize to tray");
            is_exiting_ = false;
            event->ignore();
            showMinimized();
        } else {
            LOG_INFO("User chose to exit");
            is_exiting_ = true;
            
            // 停止所有定时器
            if (live_duration_timer_) live_duration_timer_->stop();
            if (stats_update_timer_) stats_update_timer_->stop();
            if (system_info_timer_) system_info_timer_->stop();
            if (encoding_timer_) encoding_timer_->stop();
            
            // 停止推流
            if (encoder_bridge_ && encoder_bridge_->is_streaming()) {
                encoder_bridge_->stop_streaming();
            }
            
            // 清理资源并退出
            cleanupSystemTray();
            event->accept();
            QMainWindow::close();
        }
    } else {
        event->ignore();
    }
}

void MainWindow::setupSystemTray() {
    // 检查系统是否支持托盘图标
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        LOG_WARNING("System tray is not available on this system");
        return;
    }
    
    // 创建托盘图标菜单
    system_tray_menu_ = new QMenu(this);
    
    // 显示窗口动作
    tray_action_show_ = new QAction("显示窗口", this);
    connect(tray_action_show_, &QAction::triggered, this, &MainWindow::onTrayShowAction);
    system_tray_menu_->addAction(tray_action_show_);
    
    // 分隔符
    system_tray_menu_->addSeparator();
    
    // 退出动作
    tray_action_exit_ = new QAction("退出程序", this);
    connect(tray_action_exit_, &QAction::triggered, this, &MainWindow::onTrayExitAction);
    system_tray_menu_->addAction(tray_action_exit_);
    
    // 创建系统托盘图标
    system_tray_icon_ = new QSystemTrayIcon(this);
    
    // 设置托盘图标
    QIcon trayIcon(":/images/Frame_icon.png");
    if (!trayIcon.isNull()) {
        system_tray_icon_->setIcon(trayIcon);
    } else {
        // 使用默认图标
        system_tray_icon_->setIcon(QIcon::fromTheme("application-default-icon"));
    }
    
    system_tray_icon_->setToolTip("直播伴侣");
    system_tray_icon_->setContextMenu(system_tray_menu_);
    
    // 连接托盘图标激活信号
    connect(system_tray_icon_, &QSystemTrayIcon::activated,
            this, &MainWindow::onTrayIconActivated);
    
    // 显示托盘图标
    system_tray_icon_->show();

    LOG_INFO("System tray icon initialized");
}

void MainWindow::setupNetworkConnections() {
    // 连接 NetworkManager 的信号
    NetworkManager* networkManager = NetworkManager::instance();

    // 插播视频转码完成通知
    connect(networkManager, &NetworkManager::insertVideoTranscoded,
            this, [this](const QString& fileId, int fileState) {
        LOG_INFO(QString("Insert video transcoded: fileId=%1, state=%2").arg(fileId).arg(fileState).toStdString());
        // 刷新插播列表
        InsertFileManager::instance()->refreshInsertFiles(current_live_item_.liveId);
    });

    // 开始执行插播视频
    connect(networkManager, &NetworkManager::startInsertVideo,
            this, [this](const QString& fileId) {
        LOG_INFO(QString("Received start insert video command: fileId=%1").arg(fileId).toStdString());
        // 检查是否已开始插播
        if (!is_insert_video_playing_) {
            // 检查文件是否已下载
            auto fileItem = InsertFileManager::instance()->getFile(fileId);
            if (fileItem && fileItem->isDownloaded()) {
                // 从 InsertFileItem 读取循环播放设置
                bool loopEnabled = fileItem->loopEnabled;
                startInsertVideoPlayback(fileId, loopEnabled);
            } else {
                LOG_WARNING("Insert video file not ready: " + fileId.toStdString());
            }
        }
    });

    // 停止插播视频
    connect(networkManager, &NetworkManager::stopInsertVideo,
            this, [this](const QString& fileId) {
        LOG_INFO(QString("Received stop insert video command: fileId=%1").arg(fileId).toStdString());
        if (is_insert_video_playing_ && current_insert_video_file_id_ == fileId) {
            stopInsertVideoPlayback();
        }
    });

    LOG_INFO("Network connections for insert video initialized");
}

void MainWindow::cleanupSystemTray() {
    if (system_tray_icon_) {
        system_tray_icon_->hide();
        delete system_tray_icon_;
        system_tray_icon_ = nullptr;
    }
    
    if (system_tray_menu_) {
        delete system_tray_menu_;
        system_tray_menu_ = nullptr;
    }
    
    tray_action_show_ = nullptr;
    tray_action_exit_ = nullptr;
}

void MainWindow::onTrayIconActivated(QSystemTrayIcon::ActivationReason reason) {
    switch (reason) {
    case QSystemTrayIcon::Trigger:
    case QSystemTrayIcon::DoubleClick:
        // 双击或单击显示窗口
        onTrayShowAction();
        break;
    case QSystemTrayIcon::MiddleClick:
        // 中键点击最小化到托盘
        showMinimized();
        break;
    default:
        break;
    }
}

void MainWindow::onTrayShowAction() {
    showNormal();
    activateWindow();
    raise();
    
    // 确保窗口在最前端显示
    setWindowFlags(windowFlags() & ~Qt::Tool);
    show();
}

void MainWindow::onTrayExitAction() {
    is_exiting_ = true;
    
    // 停止所有定时器
    if (live_duration_timer_) live_duration_timer_->stop();
    if (stats_update_timer_) stats_update_timer_->stop();
    if (system_info_timer_) system_info_timer_->stop();
    if (encoding_timer_) encoding_timer_->stop();
    
    // 停止推流
    if (encoder_bridge_ && encoder_bridge_->is_streaming()) {
        encoder_bridge_->stop_streaming();
    }
    
    // 清理资源并退出
    cleanupSystemTray();
    QMainWindow::close();
}

void MainWindow::handleExit() {
    // 根据退出偏好设置处理退出
    if (exit_preference_ == 1) {
        // 记住最小化
        LOG_INFO("Exit preference: minimize to tray");
        showMinimized();
        return;
    } else if (exit_preference_ == 2) {
        // 记住退出 - 直接退出，不显示对话框
        LOG_INFO("Exit preference: direct exit");
        is_exiting_ = true;
        
        // 停止所有定时器
        if (live_duration_timer_) live_duration_timer_->stop();
        if (stats_update_timer_) stats_update_timer_->stop();
        if (system_info_timer_) system_info_timer_->stop();
        if (encoding_timer_) encoding_timer_->stop();
        
        // 停止推流
        if (encoder_bridge_ && encoder_bridge_->is_streaming()) {
            encoder_bridge_->stop_streaming();
        }
        
        // 清理资源并退出
        cleanupSystemTray();
        close(); // 这会触发 closeEvent，但 is_exiting_ 为 true，所以会正常关闭
        return;
    }
    
    // 默认显示退出确认对话框 - closeEvent 会处理对话框
    // 这里只需触发 closeEvent 即可
    close();
}

void MainWindow::loadExitPreference() {
    QSettings settings("LiveAssistant", "Settings");
    exit_preference_ = settings.value("exitPreference", 0).toInt();
    LOG_INFO("Loaded exit preference: " + std::to_string(exit_preference_));
}

void MainWindow::saveExitPreference(int preference) {
    exit_preference_ = preference;
    QSettings settings("LiveAssistant", "Settings");
    settings.setValue("exitPreference", preference);
    LOG_INFO("Saved exit preference: " + std::to_string(preference));
}

} // namespace live_assistant
