#include "app/main_window.h"
#include "app/camera_settings.h"
#include "app/screen_share.h"
#include "app/screen_capture_selector.h"
#include "app/settings_dialog.h"
#include "app/exit_dialog.h"
#include "app/insert_video_widget.h"
#include "app/insert_file_manager.h"
#include "app/add_material_dialog.h"
#include "customwebengineview.h"
#include "http/network_manager.h"
#include <QWebEngineSettings>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QStandardPaths>
#include <QDir>
#include "http/live_item.h"
#include "media_pipeline/media_file_source.h"
#include <QScopeGuard>
#include "ui_main_window.h"
#include "scene_manager/scene_manager.h"
#include "scene_manager/source_factory.h"
#include "scene_manager/canvas.h"
#include "scene_manager/compositor.h"
#include "scene_manager/compositor_encoder_bridge.h"
#include "video_engine/video_engine.h"
#include "audio_engine/audio_engine.h"
#include "audio_engine/audio_capturer.h"
#include "scene_manager/wgc_capture_stub.h"
#include "scene_manager/capture_factory.h"
#include "scene_manager/capture_manager_iface.h"
#include "encoder/encoder.h"
#include "stream_pusher/stream_pusher.h"
#include "common/log.h"
#include "common/error.h"
#include "common/video_frame_synchronizer.h"

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
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
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

    // 延迟初始化系统托盘图标（延迟 1 秒，让窗口先稳定显示）
    QTimer::singleShot(1000, this, [this]() {
        setupSystemTray();
        LOG_INFO("System tray initialized after delay");
    });

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

    // 初始化WebView控件（在设置直播间信息后）
    // initWebEngineViews() 将在 setLiveItem 中被调用

    // 初始化WebView UI属性（按照旧项目逻辑）
    initWebEngineUI();

    LOG_INFO("MainWindow created");
}

void MainWindow::initWebEngineUI()
{
    if (!ui->webEngineView_chat || !ui->webEngineView_product) {
        return;
    }

    // 设置WebEngine native属性（按照旧项目）
    ui->webEngineView_chat->setAttribute(Qt::WA_NativeWindow, false);
    ui->webEngineView_chat->setAttribute(Qt::WA_DontCreateNativeAncestors, true);
    ui->webEngineView_product->setAttribute(Qt::WA_NativeWindow, false);
    ui->webEngineView_product->setAttribute(Qt::WA_DontCreateNativeAncestors, true);

    // 允许触摸事件
    ui->webEngineView_chat->setAttribute(Qt::WA_AcceptTouchEvents, true);
    ui->webEngineView_product->setAttribute(Qt::WA_AcceptTouchEvents, true);

    // 启用插件
    ui->webEngineView_chat->settings()->setAttribute(QWebEngineSettings::PluginsEnabled, true);
    ui->webEngineView_product->settings()->setAttribute(QWebEngineSettings::PluginsEnabled, true);

    // 配置WebView自适应布局
    auto configWebView = [&](QWebEngineView* webView) {
        webView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        webView->setMinimumSize(0, 0);
        webView->settings()->setAttribute(QWebEngineSettings::ShowScrollBars, true);
    };

    configWebView(ui->webEngineView_chat);
    configWebView(ui->webEngineView_product);

    LOG_INFO("WebEngine UI initialized");
}

void MainWindow::initWebEngineViews() {
    // WebView加载暂时注释掉，排查崩溃问题
    LOG_INFO("initWebEngineViews called - WebView loading disabled for debugging");
    return;

    /*
    if (!ui->webEngineView_chat || !ui->webEngineView_product) {
        LOG_WARNING("WebEngineView controls not found");
        return;
    }

    // 防止重复初始化：检查是否已经加载过URL
    if (!ui->webEngineView_chat->url().isEmpty() || !ui->webEngineView_product->url().isEmpty()) {
        LOG_INFO("WebEngineViews already initialized, skipping...");
        return;
    }

    // 处理token，去掉Bearer前缀（如果有）
    QString token = token_;
    if (token.startsWith("Bearer ")) {
        token = token.mid(7);
    }

    LOG_INFO("Initializing WebEngineViews with domain: " + domain_.toStdString());

    // 使用lambda函数简化CustomWebEngineView初始化（按照旧项目逻辑）
    auto initCustomWebEngine = [this, token](QWebEngineView* webView, const QString& urlStr) {
        // 转换为CustomWebEngineView
        CustomWebEngineView* customWebView = dynamic_cast<CustomWebEngineView*>(webView);
        if (customWebView) {
            // 设置Authorization Token和域名
            customWebView->setAuthorizationToken(token, domain_);
            // 设置URL
            customWebView->setCustomUrl(QUrl(urlStr));
        }
    };

    // 初始化聊天互动WebView
    if (!current_live_item_.liveId.isEmpty()) {
        QString chatUrl = "https://" + domain_ + "/livesaas/liveStream/livedetails?roomInfoId=" + current_live_item_.liveId + "&embed=onlyInfo&tab=chat";
        LOG_INFO("Chat WebView URL: " + chatUrl.toStdString());
        initCustomWebEngine(ui->webEngineView_chat, chatUrl);
    }

    // 初始化商品卡片WebView
    if (!current_live_item_.liveId.isEmpty()) {
        QString goodsUrl = "https://" + domain_ + "/livesaas/liveStream/livedetails?roomInfoId=" + current_live_item_.liveId + "&embed=onlyInfo&tab=goods";
        LOG_INFO("Product WebView URL: " + goodsUrl.toStdString());
        initCustomWebEngine(ui->webEngineView_product, goodsUrl);
    }
    */
}

MainWindow::~MainWindow() {
    LOG_INFO("MainWindow destroyed");

    // 清理系统托盘
    cleanupSystemTray();

    // Stop timers
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

    // 连接场景管理按钮
    if (ui) {
        if (ui->comboBox_scenes) {
            ui->comboBox_scenes->setVisible(true);
        }
        if (ui->pushButton_addScene) {
            ui->pushButton_addScene->setVisible(true);
            connect(ui->pushButton_addScene, &QPushButton::clicked, this, &MainWindow::on_add_scene_clicked);
        }
    }

    // 创建右键菜单按钮（添加/删除/重命名场景）
    create_scene_buttons();
}

void MainWindow::build_scene_selector() {
    if (!scene_manager_) return;

    // 使用UI中的comboBox_scenes
    if (ui && ui->comboBox_scenes) {
        ui->comboBox_scenes->setVisible(true);

        // 阻塞信号以避免触发场景切换
        ui->comboBox_scenes->blockSignals(true);

        ui->comboBox_scenes->clear();

        auto scene_names = scene_manager_->get_scene_names();
        for (const auto& name : scene_names) {
            ui->comboBox_scenes->addItem(QString::fromStdString(name));
        }

        // 设置当前选中的场景
        auto current_scene = scene_manager_->get_current_scene();
        if (current_scene) {
            int index = ui->comboBox_scenes->findText(QString::fromStdString(current_scene->get_name()));
            if (index >= 0) {
                ui->comboBox_scenes->setCurrentIndex(index);
            }
        }

        // 只在第一次时连接信号
        static bool signal_connected = false;
        if (!signal_connected) {
            connect(ui->comboBox_scenes, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, &MainWindow::on_scene_selected);
            signal_connected = true;
        }

        // 恢复信号
        ui->comboBox_scenes->blockSignals(false);
    }
}

void MainWindow::on_scene_selected(int index) {
    if (!scene_manager_ || !ui || !ui->comboBox_scenes) return;

    QString scene_name = ui->comboBox_scenes->itemText(index);
    if (scene_name.isEmpty()) return;

    // 切换场景
    scene_manager_->set_current_scene(scene_name.toStdString());

    // 同步到canvas_widget（更新预览画布）
    if (canvas_widget_) {
        canvas_widget_->set_current_scene(scene_name.toStdString());
    }

    // 同步到video_engine
    if (video_engine_) {
        video_engine_->set_current_scene(scene_manager_->get_current_scene());
    }

    // 重建场景项列表
    build_scene_list();

    // 同步到compositor
    sync_scene_to_compositor();

    LOG_INFO("Switched to scene: " + scene_name.toStdString());
}

void MainWindow::on_add_scene_clicked() {
    if (!scene_manager_) return;

    // 弹出对话框让用户输入新场景名称
    bool ok = false;
    QString new_name = QInputDialog::getText(this, "添加场景", "请输入新场景名称:",
                                              QLineEdit::Normal, "新场景", &ok);
    if (!ok || new_name.isEmpty()) return;

    // 检查是否已存在
    auto scene_names = scene_manager_->get_scene_names();
    for (const auto& name : scene_names) {
        if (name == new_name.toStdString()) {
            QMessageBox::warning(this, "错误", "场景名称已存在！");
            return;
        }
    }

    // 创建新场景
    if (scene_manager_->create_scene(new_name.toStdString()) == ErrorCode::SUCCESS) {
        // 切换到新场景
        scene_manager_->set_current_scene(new_name.toStdString());

        // 同步到canvas_widget（更新预览画布）
        if (canvas_widget_) {
            canvas_widget_->set_current_scene(new_name.toStdString());
        }

        // 重建场景选择器
        build_scene_selector();

        // 重建场景项列表
        build_scene_list();

        // 同步
        sync_scene_to_compositor();

        LOG_INFO("Created new scene: " + new_name.toStdString());
    }
}

void MainWindow::on_remove_scene_clicked() {
    if (!scene_manager_) return;

    auto scene_names = scene_manager_->get_scene_names();
    if (scene_names.size() <= 1) {
        QMessageBox::warning(this, "错误", "至少需要保留一个场景！");
        return;
    }

    auto current_scene = scene_manager_->get_current_scene();
    if (!current_scene) return;

    // 确认删除
    int ret = QMessageBox::question(this, "删除场景",
        QString("确定要删除场景 \"%1\" 吗？").arg(QString::fromStdString(current_scene->get_name())),
        QMessageBox::Yes | QMessageBox::No);

    if (ret != QMessageBox::Yes) return;

    QString scene_name = QString::fromStdString(current_scene->get_name());

    // 删除场景
    if (scene_manager_->remove_scene(scene_name.toStdString()) == ErrorCode::SUCCESS) {
        // 重建场景选择器
        build_scene_selector();

        // 重建场景项列表
        build_scene_list();

        // 同步
        sync_scene_to_compositor();

        LOG_INFO("Removed scene: " + scene_name.toStdString());
    }
}

void MainWindow::on_rename_scene_clicked() {
    if (!scene_manager_) return;

    auto current_scene = scene_manager_->get_current_scene();
    if (!current_scene) return;

    // 弹出对话框让用户输入新名称
    bool ok = false;
    QString new_name = QInputDialog::getText(this, "重命名场景", "请输入新场景名称:",
                                              QLineEdit::Normal,
                                              QString::fromStdString(current_scene->get_name()), &ok);
    if (!ok || new_name.isEmpty()) return;

    // 检查是否已存在
    auto scene_names = scene_manager_->get_scene_names();
    for (const auto& name : scene_names) {
        if (name == new_name.toStdString() && name != current_scene->get_name()) {
            QMessageBox::warning(this, "错误", "场景名称已存在！");
            return;
        }
    }

    // 重命名
    if (current_scene->set_name(new_name.toStdString()) == ErrorCode::SUCCESS) {
        // 重建场景选择器
        build_scene_selector();

        LOG_INFO("Renamed scene to: " + new_name.toStdString());
    }
}

void MainWindow::create_scene_buttons() {
    // 为场景选择器添加右键菜单
    if (ui && ui->comboBox_scenes) {
        ui->comboBox_scenes->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(ui->comboBox_scenes, &QComboBox::customContextMenuRequested, this, [this](const QPoint& pos) {
            QMenu menu(this);

            QAction* addAction = new QAction("添加场景", &menu);
            connect(addAction, &QAction::triggered, this, &MainWindow::on_add_scene_clicked);
            menu.addAction(addAction);

            QAction* renameAction = new QAction("重命名场景", &menu);
            connect(renameAction, &QAction::triggered, this, &MainWindow::on_rename_scene_clicked);
            menu.addAction(renameAction);

            QAction* removeAction = new QAction("删除场景", &menu);
            connect(removeAction, &QAction::triggered, this, &MainWindow::on_remove_scene_clicked);
            menu.addAction(removeAction);

            menu.exec(QCursor::pos());
        });
    }
}

void MainWindow::save_scenes_config() {
    if (!scene_manager_) return;

    // 获取配置目录 - Local (新路径)
    QString config_dir_local = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir dir_local(config_dir_local);
    if (!dir_local.exists()) {
        dir_local.mkpath(config_dir_local);
    }

    // 获取配置目录 - Roaming (旧路径，兼容旧版本)
    QString config_dir_roaming = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir dir_roaming(config_dir_roaming);
    if (!dir_roaming.exists()) {
        dir_roaming.mkpath(config_dir_roaming);
    }

    // 按直播间名称保存场景配置
    QString config_file_local;
    QString config_file_roaming;
    if (!live_id_.isEmpty()) {
        config_file_local = config_dir_local + "/scenes_" + live_id_ + ".json";
        config_file_roaming = config_dir_roaming + "/scenes_" + live_id_ + ".json";
    } else {
        config_file_local = config_dir_local + "/scenes_default.json";
        config_file_roaming = config_dir_roaming + "/scenes_default.json";
    }

    // 序列化场景
    QJsonArray scenes_array = scene_manager_->serialize();

    // 构建完整的配置对象，包含场景和元数据
    QJsonObject config_obj;
    config_obj["scenes"] = scenes_array;
    config_obj["is_portrait"] = is_portrait_mode_;

    QJsonDocument doc(config_obj);
    QByteArray json_data = doc.toJson(QJsonDocument::Indented);

    // 保存到 Local 目录
    QFile file_local(config_file_local);
    if (file_local.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file_local.write(json_data);
        file_local.close();
        LOG_INFO("Scenes config saved to: " + config_file_local.toStdString());
    } else {
        LOG_ERROR("Failed to save scenes config to: " + config_file_local.toStdString());
    }

    // 同时保存到 Roaming 目录（兼容旧版本）
    QFile file_roaming(config_file_roaming);
    if (file_roaming.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file_roaming.write(json_data);
        file_roaming.close();
        LOG_INFO("Scenes config saved to (roaming): " + config_file_roaming.toStdString());
    } else {
        LOG_ERROR("Failed to save scenes config to (roaming): " + config_file_roaming.toStdString());
    }
}

void MainWindow::load_scenes_config() {
    if (!scene_manager_) return;

    // 获取配置目录 - Local
    QString config_dir_local = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir dir_local(config_dir_local);
    if (!dir_local.exists()) {
        dir_local.mkpath(config_dir_local);
    }

    // 获取配置目录 - Roaming (旧配置位置)
    QString config_dir_roaming = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir dir_roaming(config_dir_roaming);
    if (!dir_roaming.exists()) {
        dir_roaming.mkpath(config_dir_roaming);
    }

    // 构建配置文件路径
    QString config_file;
    QString live_id = live_id_;
    if (!live_id.isEmpty()) {
        config_file = config_dir_local + "/scenes_" + live_id + ".json";
    } else {
        config_file = config_dir_local + "/scenes_default.json";
    }

    LOG_INFO("Config directory (Local): " + config_dir_local.toStdString());
    LOG_INFO("Config directory (Roaming): " + config_dir_roaming.toStdString());
    LOG_INFO("Looking for config file: " + config_file.toStdString());

    // 检查是否有直播间特定的配置文件
    QFile file(config_file);
    if (!file.exists()) {
        // 检查旧格式配置文件（Local）
        QString old_file_local = config_dir_local + "/scenes.json";
        LOG_INFO("Checking legacy file (Local): " + old_file_local.toStdString());
        QFile old_file_check1(old_file_local);
        if (old_file_check1.exists()) {
            config_file = old_file_local;
            LOG_INFO("Using legacy scenes config file (Local)");
        } else {
            // 检查旧格式配置文件（Roaming/LiveAssistant子目录）
            QString old_file_roaming = config_dir_roaming + "/LiveAssistant/scenes.json";
            LOG_INFO("Checking legacy file (Roaming): " + old_file_roaming.toStdString());
            QFile old_file_check2(old_file_roaming);
            if (old_file_check2.exists()) {
                config_file = old_file_roaming;
                LOG_INFO("Using legacy scenes config file (Roaming)");
            } else {
                LOG_INFO("No scenes config file found, using default scene");
                return;
            }
        }
    }

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        LOG_ERROR("Failed to open scenes config file: " + config_file.toStdString());
        return;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError parse_error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        LOG_ERROR("Failed to parse scenes config: " + parse_error.errorString().toStdString());
        return;
    }

    QJsonArray scenes_array;
    bool loaded_portrait_mode = false;

    // 支持两种格式：1. 新格式 {scenes: [...], is_portrait: true}  2. 旧格式 [...]
    if (doc.isObject()) {
        QJsonObject config_obj = doc.object();
        if (config_obj.contains("scenes")) {
            scenes_array = config_obj["scenes"].toArray();
            if (config_obj.contains("is_portrait")) {
                loaded_portrait_mode = config_obj["is_portrait"].toBool(false);
                LOG_INFO("Loaded portrait mode from config: " + std::to_string(loaded_portrait_mode));
            }
        } else {
            // 没有 scenes 字段，可能是旧格式
            scenes_array = config_obj.toVariantMap().value("scenes").toJsonArray();
        }
    } else if (doc.isArray()) {
        scenes_array = doc.array();
    } else {
        LOG_ERROR("Scenes config is not a valid format");
        return;
    }

    // 反序列化场景
    if (scene_manager_->deserialize(scenes_array) == ErrorCode::SUCCESS) {
        // 应用保存的横竖屏状态
        if (loaded_portrait_mode != is_portrait_mode_) {
            LOG_INFO("Restoring portrait mode: " + std::to_string(loaded_portrait_mode));
            if (loaded_portrait_mode) {
                set_portrait_mode();
            } else {
                set_landscape_mode();
            }
        }

        // 重建UI
        build_scene_selector();
        build_scene_list();
        sync_scene_to_compositor();

        LOG_INFO("Scenes config loaded from: " + config_file.toStdString());
    } else {
        LOG_ERROR("Failed to deserialize scenes config");
    }
}

void MainWindow::restore_capture_sources() {
    LOG_INFO("========== restore_capture_sources START ==========");

    if (!scene_manager_ || !capture_manager_) {
        LOG_WARNING("SceneManager or CaptureManager not ready, skipping restore");
        return;
    }

    // 遍历所有场景的 SceneItem，为摄像头和屏幕共享源重建采集连接
    auto scene_names = scene_manager_->get_scene_names();
    LOG_INFO("Restoring capture sources for " + std::to_string(scene_names.size()) + " scenes");

    for (const auto& scene_name : scene_names) {
        auto items = scene_manager_->get_scene_items(scene_name);
        LOG_INFO("Scene '" + scene_name + "' has " + std::to_string(items.size()) + " items");

        for (const auto& item : items) {
            if (!item) continue;

            const std::string source_id = item->get_source_id();
            QString qsource_id = QString::fromStdString(source_id);

            // 判断源类型
            bool is_camera = qsource_id.startsWith("camera_");
            bool is_screen = qsource_id.startsWith("capture_");

            if (!is_camera && !is_screen) continue;

            // 检查是否已经有对应的采集源在运行
            if (capture_manager_->has_source(source_id)) {
                LOG_INFO("Capture source already exists: " + source_id);
                continue;
            }

            // 获取保存的设备ID和参数
            std::string device_id = item->get_device_id();
            const auto& params = item->get_source_params();

            if (device_id.empty()) {
                // 从 source_id 解析设备ID（兼容旧格式）
                if (is_camera) {
                    device_id = qsource_id.mid(7).toStdString(); // 去掉 "camera_" 前缀
                } else if (is_screen) {
                    device_id = qsource_id.mid(8).toStdString(); // 去掉 "capture_" 前缀
                }
            }

            LOG_INFO("Restoring capture source: " + source_id + ", device_id: " + device_id);

            // 创建采集配置
            CaptureConfig cfg;
            if (is_camera) {
                cfg.type = CaptureConfig::TargetType::CAMERA;
                cfg.target_id = device_id;

                // 从保存的参数中获取配置
                if (params.count("resolution")) {
                    std::string resolution = params.at("resolution");
                    size_t pos = resolution.find('x');
                    if (pos != std::string::npos) {
                        cfg.width = std::stoi(resolution.substr(0, pos));
                        cfg.height = std::stoi(resolution.substr(pos + 1));
                    }
                }
                if (params.count("fps")) {
                    cfg.fps = std::stoi(params.at("fps"));
                } else {
                    cfg.fps = 30;
                }
                if (params.count("pixel_format")) {
                    cfg.pixel_format = string_to_pixel_format(params.at("pixel_format"));
                } else {
                    cfg.pixel_format = PixelFormat::YUY2;
                }
                if (params.count("capture_mode")) {
                    cfg.capture_mode = string_to_capture_mode(params.at("capture_mode"));
                } else {
                    cfg.capture_mode = CaptureMode::FFMPEG;  // 默认 FFmpeg
                }
                if (params.count("mirror")) {
                    cfg.mirror = (params.at("mirror") == "true");
                }

                LOG_INFO("恢复摄像头配置: resolution=" + cfg.resolution_string() +
                         ", fps=" + std::to_string(cfg.fps) +
                         ", pixel_format=" + pixel_format_to_string(cfg.pixel_format) +
                         ", capture_mode=" + capture_mode_to_string(cfg.capture_mode));
            } else if (is_screen) {
                // 判断是屏幕还是窗口
                bool is_screen_mode = true;
                // 尝试从参数中获取类型
                // 这里简化处理，默认都是屏幕共享
                cfg.type = CaptureConfig::TargetType::SCREEN;
                cfg.target_id = device_id;
                cfg.fps = 15;
                if (params.count("fps")) {
                    cfg.fps = std::stoi(params.at("fps"));
                }
            }

            // 创建真正的采集源
            std::shared_ptr<ICaptureSource> src;
            try {
                src = CaptureFactory::create_capture_source(cfg);
            } catch (const std::exception& ex) {
                LOG_ERROR("Failed to create capture source: " + source_id + ", error: " + ex.what());
                continue;
            } catch (...) {
                LOG_ERROR("Failed to create capture source: " + source_id + ", unknown error");
                continue;
            }

            if (!src) {
                LOG_ERROR("Failed to create capture source: " + source_id);
                continue;
            }

            // 连接 frameReady 信号到 compositor 更新
            // 这里复用手动的 on_select_camera 中的逻辑
            connect(src.get(), &ICaptureSource::frameReady, this, [this, source_id](const CaptureFrame& frame) {
                // 更新compositor（用于推流和预览）
                if (compositor_ && !frame.image.isNull()) {
                    if (!compositor_->has_layer(source_id)) {
                        compositor_->add_layer(source_id);

                        // 同步正确的图层顺序和transform
                        if (scene_manager_) {
                            auto scene_names = scene_manager_->get_scene_names();
                            for (const auto& scene_name : scene_names) {
                                auto items = scene_manager_->get_scene_items(scene_name);
                                for (auto& item : items) {
                                    if (item && item->get_source_id() == source_id) {
                                        // 设置图层顺序
                                        compositor_->set_layer_order(source_id, item->get_order());

                                        // 设置图层transform
                                        const auto& tr = item->get_transform();
                                        int w = tr.width > 0 ? tr.width : canvas_config_.get_width();
                                        int h = tr.height > 0 ? tr.height : canvas_config_.get_height();
                                        compositor_->update_layer_transform(source_id,
                                            QRectF(tr.x, tr.y, w, h), tr.opacity);
                                        compositor_->set_layer_visible(source_id, item->is_visible());
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    compositor_->updateLayerImage(QString::fromStdString(source_id), frame.image);
                }

                // 同时更新 SceneSource（用于预览显示）
                if (scene_manager_) {
                    // 遍历所有场景的 SceneItem
                    auto scene_names = scene_manager_->get_scene_names();
                    for (const auto& scene_name : scene_names) {
                        auto items = scene_manager_->get_scene_items(scene_name);
                        for (const auto& item : items) {
                            if (item && item->get_source_id() == source_id) {
                                auto screenSrc = std::dynamic_pointer_cast<ScreenSource>(item->get_source());
                                if (screenSrc) {
                                    screenSrc->push_frame(frame.image);
                                }
                                auto cameraSrc = std::dynamic_pointer_cast<CameraSource>(item->get_source());
                                if (cameraSrc) {
                                    cameraSrc->push_frame(frame.image);
                                }
                                break;
                            }
                        }
                    }
                }
            });

            // 将采集源添加到 CaptureManager（与手动添加摄像头时的逻辑一致）
            if (capture_manager_) {
                capture_manager_->add_source(source_id, src);
            }

            // 启动采集源（CaptureFactory::create_capture_source 已经调用过 initialize，不要重复调用）
            if (src->start()) {
                LOG_INFO("Successfully restored capture source: " + source_id);
            } else {
                LOG_WARNING("Failed to start capture source: " + source_id);
            }
        }
    }

    LOG_INFO("========== restore_capture_sources END ==========");
}

void MainWindow::stop_all_capture_sources() {
    LOG_INFO("========== stop_all_capture_sources START ==========");

    // 先停止主窗口的摄像头预览（如果有）
    if (is_camera_preview_) {
        stop_camera_preview();
    }

    // 先清理 SceneManager 中的 Source（反序列化时创建的）
    if (scene_manager_) {
        scene_manager_->cleanup_all_sources();
    }

    // 然后清理 CaptureManager 中的采集源（restore_capture_sources 时创建的）
    if (!capture_manager_) {
        LOG_INFO("CaptureManager not initialized, nothing to stop");
        LOG_INFO("========== stop_all_capture_sources END ==========");
        return;
    }

    // 获取所有采集源的 ID
    auto source_ids = capture_manager_->get_all_source_ids();
    LOG_INFO("Stopping " + std::to_string(source_ids.size()) + " capture sources");

    for (const auto& source_id : source_ids) {
        LOG_INFO("Stopping capture source: " + source_id);
        capture_manager_->remove_source(source_id);
    }

    LOG_INFO("========== stop_all_capture_sources END ==========");
}

void MainWindow::build_scene_list() {
    if (!listWidget_sceneItems_) return;
    if (!scene_manager_ || !scene_manager_->get_current_scene()) {
        listWidget_sceneItems_->blockSignals(true);
        listWidget_sceneItems_->clear();
        listWidget_sceneItems_->blockSignals(false);
        updateStagePlaceholderVisibility();
        return;
    }

    // 阻断信号传播，避免在构建过程中触发不必要的更新
    listWidget_sceneItems_->blockSignals(true);
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

        QString display_name = extract_source_name(item->get_source(), item);
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

        QString display_name = extract_source_name(item->get_source(), item);
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

    // 恢复信号
    listWidget_sceneItems_->blockSignals(false);

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

void MainWindow::set_local_stream_mode(bool enabled) {
    is_local_stream_mode_ = enabled;
    LOG_INFO(QString("Local stream mode: %1").arg(enabled ? "enabled" : "disabled").toStdString());

    // 本地推流模式下禁用插播视频按钮
    if (enabled && ui->pushButton_insertVideo) {
        ui->pushButton_insertVideo->setEnabled(false);
        ui->pushButton_insertVideo->setToolTip("本地推流模式不支持插播视频");
        LOG_INFO("Insert video button disabled for local stream mode");
    }
}

void MainWindow::setCredentials(const QString& socketUrl, const QString& userId, const QString& token, const QString& liveurl, const QString& oncekey) {
    socket_url_ = socketUrl;
    user_id_ = userId;
    token_ = token;
    live_url_ = liveurl;
    once_key_ = oncekey;

    // 从socket_url提取domain（按照旧项目逻辑）
    {
        int protocolEndPos = socket_url_.indexOf("://");
        if (protocolEndPos == -1) {
            protocolEndPos = 0;
        } else {
            protocolEndPos += 3;
        }

        int portSepPos = socket_url_.indexOf(":", protocolEndPos);
        if (portSepPos != -1) {
            domain_ = socket_url_.mid(protocolEndPos, portSepPos - protocolEndPos);
        } else {
            domain_ = socket_url_.mid(protocolEndPos);
        }

        // 硬编码覆盖（和旧项目一致）
        domain_ = "b-test.lxi-tech.com";
        LOG_INFO("Extracted domain: " + domain_.toStdString());
    }

    LOG_INFO(QString("Credentials set - userId: %1, liveUrl: %2, socketUrl: %3")
        .arg(userId).arg(liveurl).arg(socketUrl).toStdString());
}

void MainWindow::setLiveItem(const LiveItem& liveItem) {
    LOG_INFO("========== setLiveItem START ==========");

    // 同步 live_id_ 与 LiveItem
    bool live_id_changed = !liveItem.liveId.isEmpty() && live_id_ != liveItem.liveId;
    if (live_id_changed) {
        LOG_INFO(QString("Live ID changed: %1 -> %2").arg(live_id_).arg(liveItem.liveId).toStdString());
    }

    current_live_item_ = liveItem;

    LOG_INFO(QString("LiveItem set - liveId: %1, title: %2, status: %3")
        .arg(liveItem.liveId)
        .arg(liveItem.title)
        .arg(liveItem.status == LiveStatus::LIVE ? "直播中" :
            liveItem.status == LiveStatus::PENDING ? "待开播" : "已结束")
        .toStdString());

    // 如果 live_id 发生变化，清理旧场景列表 UI 并准备重新加载
    if (live_id_changed) {
        live_id_ = liveItem.liveId;

        // 清理场景列表 UI
        if (listWidget_sceneItems_) {
            listWidget_sceneItems_->blockSignals(true);
            listWidget_sceneItems_->clear();
            listWidget_sceneItems_->blockSignals(false);
        }

        // 重新加载该直播间的场景配置
        load_scenes_config();
    }

    // 如果有推流地址，自动设置RTMP目标
    if (!liveItem.pushUrl.isEmpty() && !liveItem.pushUrl[0].isEmpty()) {
        QString rtmpUrl = liveItem.pushUrl[0];
        // 解析RTMP地址（假设格式为 rtmp://server/app/stream_key）
        LOG_INFO(QString("Auto-set RTMP URL from LiveItem: %1").arg(rtmpUrl).toStdString());
        // 这里可以根据需要进一步解析 server_url 和 stream_key
        rtmp_server_url_ = rtmpUrl;
    }

    // 根据服务器配置设置画布方向（当前默认使用横屏，忽略服务器竖屏配置）
    // 后续服务器API准备好后可以取消注释下面的代码
    /*
    if (!liveItem.canvasOrientation.isEmpty()) {
        LOG_INFO(QString("Server canvas orientation: %1").arg(liveItem.canvasOrientation).toStdString());
        apply_server_canvas_config(liveItem.canvasOrientation);
    } else {
        LOG_INFO("No canvas orientation from server, using default landscape mode");
        // 默认使用横屏
        apply_server_canvas_config("landscape");
    }
    */
    // 临时：强制使用横屏模式，等待服务器API完善
    LOG_INFO("Using default landscape mode (ignoring server config for now)");
    apply_server_canvas_config("landscape");

    // 更新切换按钮文本
    if (ui->pushButton_toggleOrientation) {
        ui->pushButton_toggleOrientation->setText(is_portrait_mode_ ? "竖屏" : "横屏");
    }

    // 初始化WebView控件
    initWebEngineViews();
    LOG_INFO("========== setLiveItem END ==========");
}

void MainWindow::initialize_modules() {
    LOG_INFO("========== initialize_modules START ==========");

    // 清理旧的模块资源（如果有）
    if (capture_manager_) {
        auto source_ids = capture_manager_->get_all_source_ids();
        for (const auto& id : source_ids) {
            capture_manager_->remove_source(id);
        }
    }
    capture_manager_ = std::make_shared<CaptureManagerIface>();
    scene_manager_ = std::make_shared<SceneManager>();
    video_engine_ = std::make_shared<VideoEngine>();
    audio_engine_ = std::make_shared<AudioEngine>();
    encoder_ = std::make_shared<Encoder>();
    stream_pusher_ = std::make_shared<StreamPusher>();

    compositor_ = std::make_shared<Compositor>();
    encoder_bridge_ = std::make_shared<CompositorEncoderBridge>();

    // 初始化场景选择器UI
    build_scene_selector();

    // 加载保存的场景配置
    load_scenes_config();

    // 使用当前画布配置的分辨率初始化视频引擎
    LOG_INFO(QString("Initializing VideoEngine with resolution: %1x%2")
        .arg(canvas_config_.get_width()).arg(canvas_config_.get_height()).toStdString());
    video_engine_->initialize(canvas_config_.get_width(), canvas_config_.get_height(), 30);
    audio_engine_->initialize(48000, 2);
    // 进入直播间时不立即启动麦克风采集，等点击"开始直播"时再采集
    // 这样可以避免持续占用麦克风资源
    LOG_INFO("Audio engine initialized, will start capture when live streaming begins");
    update_audio_status("待机", "gray");
    // 先设置静音模式，等开始直播时再启动真实采集
    if (encoder_bridge_) {
        encoder_bridge_->set_audio_engine(audio_engine_);
        encoder_bridge_->set_silent_audio(true);  // 默认静音，等开始直播后开启
    }

    // 加载保存的音量设置（如果有），否则使用系统当前音量
    loadAudioVolumeSettings();

    // 初始化音频控件UI（同步滑块值）
    update_microphone_ui();
    update_speaker_ui();

    video_engine_->set_current_scene(scene_manager_->get_current_scene());

    VideoEncoderConfig video_config;
    // 使用当前画布配置的分辨率初始化编码器
    video_config.width = canvas_config_.get_width();
    video_config.height = canvas_config_.get_height();
    video_config.fps = 30;
    video_config.bitrate = is_portrait_mode_ ? 2000000 : 2500000;  // 竖屏适当降低码率
    video_config.gop = 60; // Reduce GOP size for faster keyframe interval (2 seconds at 30fps)
    video_config.b_frames_enabled = false;
    LOG_INFO(QString("Initializing video encoder: %1x%2").arg(video_config.width).arg(video_config.height).toStdString());
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

    // 程序启动时启动系统信息更新定时器（非直播状态，每3秒更新）
    // 注意：延迟启动，确保其他模块已完全初始化
    if (system_info_timer_) {
        QTimer::singleShot(500, this, [this]() {
            if (system_info_timer_) {
                system_info_timer_->start(3000);
                // 立即更新一次显示
                update_system_info();
            }
        });
    }

    LOG_INFO("========== initialize_modules END ==========");
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
            dlg.set_video_engine(video_engine_);

            // 获取当前摄像头配置
            std::string current_camera_device_id;  // 原始设备ID
            std::string current_resolution = video_engine_->get_camera_resolution();
            int current_fps = video_engine_->get_camera_fps();
            bool current_mirror = video_engine_->get_camera_mirror();

            // 从当前场景中获取摄像头信息
            if (scene_manager_ && scene_manager_->get_current_scene()) {
                auto scene = scene_manager_->get_current_scene();
                auto items = scene->get_all_scene_items();
                for (const auto& item : items) {
                    if (item && item->get_source() &&
                        QString::fromStdString(item->get_source()->get_id()).startsWith("camera_")) {
                        // 获取原始设备ID（用于匹配下拉框）
                        current_camera_device_id = item->get_device_id();
                        // 从 SceneItem 的 Transform 中获取镜像状态
                        auto transform = item->get_transform();
                        current_mirror = transform.mirror;
                        break;
                    }
                }
            }

            // 设置当前摄像头配置
            dlg.set_camera_config(current_camera_device_id, current_resolution, current_fps, current_mirror);
        }

        // 隐藏背景tab页（功能未实现）
        dlg.hide_background_tab();

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

    // 横竖屏切换按钮连接
    if (ui->pushButton_toggleOrientation) {
        connect(ui->pushButton_toggleOrientation, &QPushButton::clicked, this, [this]() {
            toggle_canvas_orientation();
            // 更新按钮文本
            if (ui->pushButton_toggleOrientation) {
                ui->pushButton_toggleOrientation->setText(is_portrait_mode_ ? "竖屏" : "横屏");
            }
        });
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
                        // 本地推流模式不支持插播视频
                        if (is_local_stream_mode_) {
                            QMessageBox::information(this, "提示", "本地推流模式不支持插播视频");
                            return;
                        }
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
                    // 停止直播时也停止音频采集
                    if (audio_engine_ && audio_engine_->is_capturing()) {
                        LOG_INFO("Stopping audio capture after live streaming ended");
                        audio_engine_->stop_capture();
                    }
                    ui->pushButton_startLive->setText("开始直播");
                    // 启用画布切换按钮
                    if (ui->pushButton_toggleOrientation) {
                        ui->pushButton_toggleOrientation->setEnabled(true);
                        ui->pushButton_toggleOrientation->setStyleSheet(
                            "QPushButton { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #4a6ef0, stop:1 #6a8ef0); color: white; border: none; border-radius: 4px; font-size: 12px; font-weight: bold; }"
                            "QPushButton:hover { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #5a7ef0, stop:1 #7a9ef0); }"
                            "QPushButton:disabled { background: #666666; color: #999999; }"
                        );
                    }
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
                    // 改为3秒间隔持续更新系统信息（非直播状态）
                    if (system_info_timer_) {
                        system_info_timer_->start(3000);
                        // 立即更新一次显示
                        update_system_info();
                    }
                    LOG_INFO("直播已结束");
                }
                // 如果用户选择"否"，什么都不做
                return;
            }

            // 开始推流 - 显示开始直播确认对话框
            LOG_DEBUG("[DIAG] 准备开始推流");
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
                    // 开始直播时启动音频采集
                    if (audio_engine_ && !audio_engine_->is_capturing()) {
                        LOG_INFO("Starting audio capture for live streaming");
                        update_audio_status("初始化中...", "orange");
                        bool audio_started = false;
                        int retry_count = 3;
                        for (int i = 0; i < retry_count && !audio_started; ++i) {
                            if (i > 0) {
                                QThread::msleep(500);
                            }
                            if (audio_engine_->start_capture()) {
                                audio_started = true;
                            }
                        }
                        if (!audio_started) {
                            LOG_WARNING("Failed to start audio capture for live streaming");
                            update_audio_status("故障", "red");
                        } else {
                            // 启动成功后，关闭静音模式
                            if (encoder_bridge_) {
                                encoder_bridge_->set_silent_audio(false);
                            }
                            update_audio_status("正常", "green");
                        }
                    }

                    ui->pushButton_startLive->setText("停止直播");
                    // 禁用画布切换按钮
                    if (ui->pushButton_toggleOrientation) {
                        ui->pushButton_toggleOrientation->setEnabled(false);
                        ui->pushButton_toggleOrientation->setStyleSheet(
                            "QPushButton { background: #666666; color: #999999; border: none; border-radius: 4px; font-size: 12px; font-weight: bold; }"
                        );
                    }
                    if (ui->label_status) {
                        ui->label_status->setText("正在推流");
                    }
                    // 启动直播时长计时器
                    streaming_start_time_ms_ = QDateTime::currentMSecsSinceEpoch();
                    // 直播时长显示用更高频刷新，避免偶尔“跳两秒”的观感（实际时长仍按系统时钟计算）
                    if (live_duration_timer_) {
                        live_duration_timer_->start(200);
                    }
                    // 系统监控（CPU/内存/GPU/码率/FPS）按 1 秒更新
                    if (system_info_timer_) {
                        system_info_timer_->start(1000);
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
            playVolumeFeedbackSound();
        });
        // 滑块释放时保存设置
        connect(ui->slider_mic, &QSlider::sliderReleased, this, [this]() {
            saveAudioVolumeSettings();
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
            playVolumeFeedbackSound();
        });
        // 滑块释放时保存设置
        connect(ui->slider_speaker, &QSlider::sliderReleased, this, [this]() {
            saveAudioVolumeSettings();
        });
    }

    // Initialize audio controls
    update_microphone_ui();
    update_speaker_ui();

    // 插播视频按钮连接（本地推流模式下禁用）
    if (ui->pushButton_insertVideo) {
        if (is_local_stream_mode_) {
            // 本地推流模式：禁用插播视频按钮
            ui->pushButton_insertVideo->setEnabled(false);
            ui->pushButton_insertVideo->setToolTip("本地推流模式不支持插播视频");
            LOG_INFO("Insert video button disabled for local stream mode");
        } else {
            connect(ui->pushButton_insertVideo, &QPushButton::clicked, this, &MainWindow::on_insert_video_button_clicked);
        }
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

    // 设置: #9C27B0 -> #E040FB (紫色)
    QString settingsStyle = R"(
        QPushButton {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, 
                                        stop:0 #9C27B0, stop:1 #E040FB);
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
                                        stop:0 #AB47BC, stop:1 #E91E63);
        }
        QPushButton:pressed {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, 
                                        stop:0 #7B1FA2, stop:1 #C2185B);
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
    if (ui->pushButton_settings) {
        ui->pushButton_settings->setStyleSheet(settingsStyle);
    }
    
    LOG_INFO("Bottom buttons styles applied with different colors");
}

void MainWindow::on_insert_video_button_clicked() {
    LOG_INFO("Insert video button clicked");

    // 本地推流模式不支持插播视频
    if (is_local_stream_mode_) {
        QMessageBox::information(this, "提示", "本地推流模式不支持插播视频");
        return;
    }

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

    // 设置音频回调：直接推送到 AudioEngine 队列
    mediaSource->set_audio_ready_callback([this, source_id](std::shared_ptr<AudioFrame> frame) {
        if (audio_engine_ && frame) {
            audio_engine_->pushMediaFrame(frame);
        }
    });

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

        // 设置帧回调 - 通过 encoder_bridge 设置无锁回调
        // 注意：不再使用 Qt 信号槽，因为回调已经在工作线程中直接更新 latest_frame_
        // 画布渲染时通过 get_latest_frame() 获取帧（和摄像头/屏幕共享一样）
        if (encoder_bridge_) {
            encoder_bridge_->attach_insert_video_source(mediaSource.get());
        }

        // 启动播放
        if (mediaSource->start()) {
            current_insert_video_source_ = mediaSource;
            current_insert_video_file_id_ = fileId;
            is_insert_video_playing_ = true;

            // 创建帧同步定时器（30fps，每33ms从同步器取帧更新到Compositor）
            insert_video_timer_ = new QTimer(this);
            connect(insert_video_timer_, &QTimer::timeout, this, &MainWindow::on_insert_video_frame_ready);
            insert_video_timer_->start(33);  // ~30fps
            LOG_INFO("[INSERT_VIDEO] Frame sync timer started at 30fps");

            // 设置混音模式：麦克风 + 插播音频
            if (audio_engine_) {
                audio_engine_->setMixMode(AudioMixMode::MIC_MEDIA);
                audio_engine_->set_media_volume(0.7f);  // 默认插播音量为70%

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

    // 停止帧同步定时器
    if (insert_video_timer_) {
        insert_video_timer_->stop();
        insert_video_timer_->deleteLater();
        insert_video_timer_ = nullptr;
    }

    // 分离插播视频源（清除时间基准和同步器）
    if (encoder_bridge_ && current_insert_video_source_) {
        encoder_bridge_->detach_insert_video_source(current_insert_video_source_.get());
    }

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

void MainWindow::on_insert_video_frame_ready() {
    // 从 encoder_bridge_ 的同步器获取插播视频帧
    if (!encoder_bridge_ || !compositor_ || !current_insert_video_source_) {
        return;
    }

    SyncedVideoFrame synced_frame;
    // 非阻塞获取帧（最多尝试一次）
    if (encoder_bridge_->pop_insert_video_frame(synced_frame) && synced_frame.frame) {
        const std::string source_id = current_insert_video_source_->get_id();
        // 更新 Compositor 中的层（这会同时更新画布显示和推流）
        compositor_->update_layer_video_frame(source_id, synced_frame.frame);

        // 调试日志：确认帧更新成功
        static int frame_count = 0;
        static int64_t last_log_time = 0;
        frame_count++;
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - last_log_time >= 1000) {
            LOG_INFO("[INSERT_VIDEO] Compositor frame update: " + std::to_string(frame_count) + " fps");
            frame_count = 0;
            last_log_time = now;
        }
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

    // 检查当前是否有摄像头源在运行
    std::string existing_camera_device_id;
    bool has_running_camera = false;
    if (capture_manager_) {
        auto source_ids = capture_manager_->get_all_source_ids();
        for (const auto& sid : source_ids) {
            if (sid.rfind("camera_", 0) == 0) {
                has_running_camera = true;
                LOG_INFO("找到已有摄像头源在运行: " + sid);
                break;
            }
        }
    }

    CameraSettingsDialog dialog(this);

    // 如果已经有摄像头在运行，禁用预览功能避免冲突
    if (has_running_camera) {
        LOG_INFO("已有摄像头在运行，禁用预览功能");
        dialog.set_preview_disabled(true);
    }

    // 使用新的接口传递摄像头信息（包含 OpenCV 索引）
    std::vector<std::string> display_names;
    std::vector<std::string> dshow_names;
    std::vector<int> opencv_indices;
    for (const auto& choice : camera_choices) {
        display_names.push_back(choice.display_name);
        dshow_names.push_back(choice.dshow_name);
        opencv_indices.push_back(choice.opencv_index);
    }
    dialog.set_available_cameras_with_opencv(display_names, dshow_names, opencv_indices);

    if (dialog.exec() == QDialog::Accepted) {
        // 获取完整的采集配置
        CaptureConfig capture_cfg = dialog.get_capture_config();

        const std::string camera_device_id = capture_cfg.target_id;
        QString selected_camera = QString::fromStdString(capture_cfg.display_name);
        int opencv_index = capture_cfg.opencv_index;

        LOG_INFO("选中摄像头: " + selected_camera.toStdString() +
                 ", OpenCV index: " + std::to_string(opencv_index));
        LOG_INFO("摄像头参数 - 分辨率: " + capture_cfg.resolution_string() +
                 ", 帧率: " + std::to_string(capture_cfg.fps) +
                 ", 像素格式: " + pixel_format_to_string(capture_cfg.pixel_format) +
                 ", 采集模式: " + capture_mode_to_string(capture_cfg.capture_mode) +
                 ", 镜像: " + (capture_cfg.mirror ? "开启" : "关闭"));

        if (video_engine_) {
            video_engine_->set_camera_resolution(capture_cfg.resolution_string());
            video_engine_->set_camera_fps(capture_cfg.fps);
            video_engine_->set_camera_pixel_format(pixel_format_to_string(capture_cfg.pixel_format));
            video_engine_->set_camera_mirror(capture_cfg.mirror);
            video_engine_->set_capture_mode(capture_cfg.capture_mode);
        }

        if (camera_device_id.empty()) {
            LOG_ERROR("摄像头设备标识无效");
            QMessageBox::warning(this, "错误", "摄像头设备标识无效");
            return;
        }

        // 检查是否已有该摄像头源
        const std::string source_id = "camera_" + std::to_string(std::hash<std::string>{}(camera_device_id));
        if (capture_manager_ && capture_manager_->has_source(source_id)) {
            LOG_INFO("该摄像头已在使用中，无需重新添加");
            QMessageBox::information(this, "提示", "该摄像头已在使用中");
            return;
        }

        // 尝试从对话框获取预览源（复用已打开的摄像头）
        auto preview_source = dialog.take_preview_source();
        if (preview_source) {
            LOG_INFO("复用对话框已打开的摄像头源");
            // 使用对话框创建的预览源
            on_select_camera_with_source(selected_camera, capture_cfg, preview_source);
        } else {
            // 对话框没有创建新源，需要创建
            LOG_INFO("创建新的摄像头源");
            on_select_camera(selected_camera, capture_cfg);
        }
    }
    // 如果取消，对话框的析构函数会自动清理临时预览源
}

void MainWindow::on_select_camera(const QString& camera_name, const CaptureConfig& config) {
    LOG_INFO("Selected camera: '" + camera_name.toStdString() +
             "' mode=" + capture_mode_to_string(config.capture_mode));

    if (!capture_manager_) {
        LOG_ERROR("采集管理器未初始化");
        QMessageBox::warning(this, "错误", "采集管理器未初始化");
        return;
    }

    // 使用配置中的设备 ID
    std::string camera_device_id = config.target_id;
    if (config.capture_mode == CaptureMode::OPENCV) {
        // OpenCV 模式使用索引
        camera_device_id = std::to_string(config.opencv_index);
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

    // 使用传入的配置
    CaptureConfig cfg = config;
    cfg.target_id = camera_device_id;

    LOG_INFO("采集配置: 类型=CAMERA, 目标ID=" + cfg.target_id +
             ", 显示名=" + cfg.display_name +
             ", 分辨率=" + cfg.resolution_string() +
             ", 帧率=" + std::to_string(cfg.fps) +
             ", 像素格式=" + pixel_format_to_string(cfg.pixel_format) +
             ", 采集模式=" + capture_mode_to_string(cfg.capture_mode) +
             ", 镜像=" + (cfg.mirror ? "开启" : "关闭") +
             ", OpenCV索引=" + std::to_string(cfg.opencv_index));

    LOG_INFO("调用CaptureFactory::create_capture_source创建采集源");
    std::shared_ptr<ICaptureSource> src;
    try {
        src = CaptureFactory::create_capture_source(cfg);
    } catch (const std::exception& ex) {
        LOG_ERROR("创建摄像头采集源时发生异常: " + std::string(ex.what()));
        QMessageBox::warning(this, "错误", "创建摄像头采集源失败: " + QString::fromStdString(ex.what()));
        return;
    } catch (...) {
        LOG_ERROR("创建摄像头采集源时发生未知异常");
        QMessageBox::warning(this, "错误", "创建摄像头采集源失败（未知错误）");
        return;
    }

    if (!src) {
        LOG_ERROR("创建摄像头采集源失败");
        QMessageBox::warning(this, "错误", "创建摄像头采集源失败");
        return;
    }
    LOG_INFO("成功创建摄像头采集源");

    LOG_INFO("连接frameReady信号到Compositor的槽函数");
    // 使用信号槽连接替代回调，显式指定跨线程连接类型
    connect(src.get(), &ICaptureSource::frameReady, this, [this, source_id](const CaptureFrame& frame) {
        auto cb_start = std::chrono::high_resolution_clock::now();
        LOG_DEBUG("[DIAG] 收到frameReady信号，源ID: " + source_id + ", 图像尺寸: " + std::to_string(frame.image.width()) + "x" + std::to_string(frame.image.height()));

        // 更新compositor（用于推流）
        if (compositor_ && !frame.image.isNull()) {
            if (!compositor_->has_layer(source_id)) {
                LOG_DEBUG("[DIAG] 图层不存在，创建新图层: " + source_id);
                compositor_->add_layer(source_id);

                // 同步正确的图层顺序和transform
                if (scene_manager_ && scene_manager_->get_current_scene()) {
                    auto scene = scene_manager_->get_current_scene();
                    auto items = scene->get_all_scene_items();
                    for (auto& item : items) {
                        if (item && item->get_source_id() == source_id) {
                            // 设置图层顺序
                            compositor_->set_layer_order(source_id, item->get_order());

                            // 设置图层transform
                            const auto& tr = item->get_transform();
                            int w = tr.width > 0 ? tr.width : canvas_config_.get_width();
                            int h = tr.height > 0 ? tr.height : canvas_config_.get_height();
                            compositor_->update_layer_transform(source_id,
                                QRectF(tr.x, tr.y, w, h), tr.opacity);
                            compositor_->set_layer_visible(source_id, item->is_visible());
                            break;
                        }
                    }
                }
            }
            auto t0 = std::chrono::high_resolution_clock::now();
            compositor_->updateLayerImage(QString::fromStdString(source_id), frame.image);
            auto t1 = std::chrono::high_resolution_clock::now();
            LOG_DEBUG("[DIAG] updateLayerImage耗时=" + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(t1-t0).count()) + "us");
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
                        auto t0 = std::chrono::high_resolution_clock::now();
                        screenSrc->push_frame(frame.image);
                        auto t1 = std::chrono::high_resolution_clock::now();
                        LOG_DEBUG("[DIAG] push_frame(ScreenSource)耗时=" + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(t1-t0).count()) + "us");
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
                        auto t0 = std::chrono::high_resolution_clock::now();
                        cameraSrc->push_frame(frame.image);
                        auto t1 = std::chrono::high_resolution_clock::now();
                        LOG_DEBUG("[DIAG] push_frame(CameraSource)耗时=" + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(t1-t0).count()) + "us");
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
                // 保存设备ID和参数，用于序列化
                added_item->set_device_id(camera_device_id);
                added_item->set_display_name(camera_name.toStdString());

                // 保存摄像头参数
                std::unordered_map<std::string, std::string> params;

                // 从 video_engine_ 获取当前配置
                if (video_engine_) {
                    params["resolution"] = video_engine_->get_camera_resolution();
                    params["fps"] = std::to_string(video_engine_->get_camera_fps());
                    params["pixel_format"] = video_engine_->get_camera_pixel_format();
                    params["capture_mode"] = capture_mode_to_string(video_engine_->get_capture_mode());
                } else {
                    params["resolution"] = "640x360";
                    params["fps"] = "30";
                    params["pixel_format"] = "YUY2";
                    params["capture_mode"] = "FFMPEG";
                }
                params["mirror"] = config.mirror ? "true" : "false";

                // 保存 FFmpeg 设备 ID（Friendly Name）和 OpenCV 索引
                params["ffmpeg_device_id"] = camera_device_id;  // FFmpeg 用 Friendly Name
                // OpenCV 索引需要从 capture_cfg 获取，但这里没有
                // 暂时保存 camera_device_id，恢复时如果是 OpenCV 模式会重新枚举
                params["opencv_index"] = "0";  // 默认索引
                added_item->set_source_params(params);

                // 使用canvas_config_的逻辑尺寸，确保视频源初始尺寸合理
                int canvas_w = canvas_config_.get_width(); // 默认1280
                int canvas_h = canvas_config_.get_height(); // 默认720
                int w = canvas_w / 2;
                int h = canvas_h / 2;
                int x = (canvas_w - w) / 2;
                int y = (canvas_h - h) / 2;

                // Get mirror setting from video engine
                bool mirror = false;
                if (video_engine_) {
                    mirror = video_engine_->get_camera_mirror();
                }

                Transform tr(x, y, w, h, 0.0f, 1.0f, mirror);
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

void MainWindow::on_select_camera_with_source(const QString& camera_name, const CaptureConfig& config, std::shared_ptr<ICaptureSource> existing_source) {
    LOG_INFO("on_select_camera_with_source: '" + camera_name.toStdString() +
             "' mode=" + capture_mode_to_string(config.capture_mode));

    if (!capture_manager_) {
        LOG_ERROR("采集管理器未初始化");
        QMessageBox::warning(this, "错误", "采集管理器未初始化");
        return;
    }

    // 使用配置中的设备 ID
    std::string camera_device_id = config.target_id;
    if (config.capture_mode == CaptureMode::OPENCV) {
        camera_device_id = std::to_string(config.opencv_index);
    }

    const std::string source_id = "camera_" + std::to_string(std::hash<std::string>{}(camera_device_id));
    LOG_INFO("生成的源ID: " + source_id);

    // 检查是否已存在
    if (capture_manager_->has_source(source_id)) {
        LOG_INFO("该摄像头已在使用中: " + source_id);
        QMessageBox::information(this, "提示", "该摄像头已在使用中");
        return;
    }

    if (!existing_source) {
        LOG_ERROR("提供的采集源无效");
        QMessageBox::warning(this, "错误", "提供的采集源无效");
        return;
    }

    // 连接frameReady信号到Compositor的槽函数
    LOG_INFO("连接frameReady信号到Compositor的槽函数");
    connect(existing_source.get(), &ICaptureSource::frameReady, this, [this, source_id](const CaptureFrame& frame) {
        LOG_DEBUG("[DIAG] 收到frameReady信号，源ID: " + source_id);

        // 更新compositor（用于推流）
        if (compositor_ && !frame.image.isNull()) {
            if (!compositor_->has_layer(source_id)) {
                compositor_->add_layer(source_id);

                // 同步正确的图层顺序和transform
                if (scene_manager_ && scene_manager_->get_current_scene()) {
                    auto scene = scene_manager_->get_current_scene();
                    auto items = scene->get_all_scene_items();
                    for (auto& item : items) {
                        if (item && item->get_source_id() == source_id) {
                            compositor_->set_layer_order(source_id, item->get_order());
                            const auto& tr = item->get_transform();
                            int w = tr.width > 0 ? tr.width : canvas_config_.get_width();
                            int h = tr.height > 0 ? tr.height : canvas_config_.get_height();
                            compositor_->update_layer_transform(source_id,
                                QRectF(tr.x, tr.y, w, h), tr.opacity);
                            compositor_->set_layer_visible(source_id, item->is_visible());
                            break;
                        }
                    }
                }
            }
            compositor_->updateLayerImage(QString::fromStdString(source_id), frame.image);
        }

        // 更新 CameraSource（用于预览显示）
        if (scene_manager_ && scene_manager_->get_current_scene()) {
            auto scene = scene_manager_->get_current_scene();
            auto items = scene->get_all_scene_items();
            for (auto& item : items) {
                if (item && item->get_source_id() == source_id) {
                    auto cameraSrc = std::dynamic_pointer_cast<CameraSource>(item->get_source());
                    if (cameraSrc) {
                        cameraSrc->push_frame(frame.image);
                    }
                    break;
                }
            }
        }
    }, Qt::QueuedConnection);
    LOG_INFO("信号槽连接成功");

    // 将采集源添加到采集管理器
    LOG_INFO("将采集源添加到采集管理器: " + source_id);
    capture_manager_->add_source(source_id, existing_source);
    LOG_INFO("采集源添加成功");

    // 将摄像头源添加到场景中
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
        if (added_item) {
            // 保存设备ID和参数
            added_item->set_device_id(camera_device_id);
            added_item->set_display_name(camera_name.toStdString());

            // 保存摄像头参数
            std::unordered_map<std::string, std::string> params;
            params["resolution"] = config.resolution_string();
            params["fps"] = std::to_string(config.fps);
            params["pixel_format"] = pixel_format_to_string(config.pixel_format);
            params["capture_mode"] = capture_mode_to_string(config.capture_mode);
            params["mirror"] = config.mirror ? "true" : "false";
            params["ffmpeg_device_id"] = config.target_id;
            params["opencv_index"] = std::to_string(config.opencv_index);
            added_item->set_source_params(params);

            int canvas_w = canvas_config_.get_width();
            int canvas_h = canvas_config_.get_height();
            int w = canvas_w / 2;
            int h = canvas_h / 2;
            int x = (canvas_w - w) / 2;
            int y = (canvas_h - h) / 2;

            Transform tr(x, y, w, h, 0.0f, 1.0f, config.mirror);
            scene->set_transform(added_item, tr);

            added_item->set_order(9999);
            scene->normalize_orders();
        }
        LOG_INFO("更新场景项并同步到Compositor");
        update_scene_items();
        LOG_INFO("Added camera source to scene: " + camera_name.toStdString() + ", id=" + source_id);
    }

    LOG_INFO("摄像头采集源复用成功: " + source_id);
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
                    // 每帧都打印会导致 UI 卡顿，仅在调试时打开
                    //LOG_INFO("   收到frameReady信号，源ID: " + source_id + ", 图像尺寸: " + std::to_string(frame.image.width()) + "x" + std::to_string(frame.image.height()));

                    // 对于屏幕共享，同时更新compositor和ScreenSource
                    if (source_id.find("capture_") == 0) {  // 屏幕共享源ID以"capture_"开头
                        // 更新compositor（用于推流）
                        if (compositor_ && !frame.image.isNull()) {
                            if (!compositor_->has_layer(source_id)) {
                                LOG_DEBUG("[DIAG] 图层不存在，创建新图层: " + source_id);
                                compositor_->add_layer(source_id);

                                // 同步正确的图层顺序和transform
                                if (scene_manager_ && scene_manager_->get_current_scene()) {
                                    auto scene = scene_manager_->get_current_scene();
                                    auto items = scene->get_all_scene_items();
                                    for (auto& item : items) {
                                        if (item && item->get_source_id() == source_id) {
                                            // 设置图层顺序
                                            compositor_->set_layer_order(source_id, item->get_order());

                                            // 设置图层transform
                                            const auto& tr = item->get_transform();
                                            int w = tr.width > 0 ? tr.width : canvas_config_.get_width();
                                            int h = tr.height > 0 ? tr.height : canvas_config_.get_height();
                                            compositor_->update_layer_transform(source_id,
                                                QRectF(tr.x, tr.y, w, h), tr.opacity);
                                            compositor_->set_layer_visible(source_id, item->is_visible());
                                            break;
                                        }
                                    }
                                }
                            }
                            LOG_DEBUG("[DIAG] 更新Compositor图层图像: " + source_id);
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
                                        LOG_DEBUG("[DIAG] 更新ScreenSource图像: " + source_id);
                                        screenSrc->push_frame(frame.image);
                                        break;
                                    }

                                    // 尝试更新 CameraSource（摄像头）
                                    auto cameraSrc = std::dynamic_pointer_cast<CameraSource>(item->get_source());
                                    if (cameraSrc) {
                                        LOG_DEBUG("[DIAG] 更新CameraSource图像: " + source_id);
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
                            // 保存设备ID和参数，用于序列化
                            added_item->set_device_id(selected_target->id);

                            // 保存屏幕共享参数
                            std::unordered_map<std::string, std::string> params;
                            params["fps"] = std::to_string(fps);
                            params["capture_cursor"] = capture_cursor ? "true" : "false";
                            params["capture_border"] = capture_border ? "true" : "false";
                            added_item->set_source_params(params);

                            // 使用canvas_config_的逻辑尺寸，让视频源自适应满画布
                            int canvas_w = canvas_config_.get_width();
                            int canvas_h = canvas_config_.get_height();

                            // 假设视频源的原始宽高比（这里使用16:9作为默认值，实际应该从视频源获取）
                            // 注意：实际应用中应该从视频源获取真实的宽高比
                            int src_w = 1280; // 假设视频源宽度
                            int src_h = 720; // 假设视频源高度

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

                        // 同步图层顺序到 Compositor
                        sync_scene_to_compositor();

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
    LOG_INFO("========== setup_canvas_widget START ==========");
    canvas_widget_ = new CanvasWidget(this);
    canvas_widget_->set_scene_manager(scene_manager_);
    canvas_widget_->set_video_engine(video_engine_);
    // 使用默认的画布配置（横屏16:9）
    LOG_INFO("Calling set_canvas_config from setup_canvas_widget");
    set_canvas_config(canvas_config_);

    canvas_widget_->set_compositor(compositor_);

    encoder_bridge_->set_compositor(compositor_);
    // 设置 CanvasRenderer 作为回退方案（用于推流捕获）
    if (canvas_widget_->get_renderer() && scene_manager_->get_current_scene()) {
        encoder_bridge_->set_canvas_renderer(
            std::shared_ptr<CanvasRenderer>(canvas_widget_->get_renderer()), 
            scene_manager_->get_current_scene()
        );
    }
    encoder_bridge_->set_encoder(encoder_);
    encoder_bridge_->set_stream_pusher(stream_pusher_);
    encoder_bridge_->set_audio_engine(audio_engine_);
    // 使用当前画布配置的分辨率，而不是硬编码
    encoder_bridge_->set_resolution(canvas_config_.get_width(), canvas_config_.get_height());
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

    // 创建画布容器，用于保持宽高比
    if (!canvasContainer_) {
        canvasContainer_ = new QWidget(ui->centralWidget);
        canvasContainer_->setStyleSheet("background-color: #000000;");
    }
    ui->verticalLayout_liveArea->insertWidget(0, canvasContainer_);

    // 将 canvas_widget_ 放入容器中
    canvas_widget_->setParent(canvasContainer_);
    // 初始时隐藏画布，等布局完成后再显示正确大小
    canvas_widget_->hide();

    // 强制布局更新，确保容器大小已计算完成
    ui->liveArea->updateGeometry();
    ui->liveArea->layout()->activate();
    canvasContainer_->updateGeometry();

    // 计算并设置正确的初始大小（基于容器大小）
    QTimer::singleShot(0, this, [this]() {
        // 先安装事件过滤器，再调整大小
        if (canvasContainer_) {
            canvasContainer_->installEventFilter(this);
        }
        update_stage_container_aspect_ratio();
        if (canvas_widget_) {
            canvas_widget_->show();
            LOG_INFO("Canvas widget shown with correct size");
        }
    });
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

    // 恢复采集源（反序列化后重建采集连接，使画面能正常显示）
    restore_capture_sources();

    LOG_INFO("========== setup_canvas_widget END ==========");
}

void MainWindow::update_preview() {
    if (canvas_widget_) {
        canvas_widget_->refresh();
    }

    // Camera frames are now driven by CaptureManager camera sources.
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
        // 根据当前画布方向调整舞台容器宽高比
        update_stage_container_aspect_ratio();
        // Reposition placeholder overlays when liveArea resizes
        repositionPlaceholderOverlays();
        return false;
    }
    // Handle canvas widget resize to reposition placeholder overlays
    if (watched == canvas_widget_ && event->type() == QEvent::Resize) {
        repositionPlaceholderOverlays();
        return false;
    }
    // Handle canvas container resize to update aspect ratio
    if (watched == canvasContainer_ && event->type() == QEvent::Resize) {
        update_stage_container_aspect_ratio();
        return false;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::update_system_info() {
    // 更新系统监控数据
    system_monitor().update();

    // 获取系统信息
    const auto& sys_stats = system_monitor().get_cached_stats();

    // 准备内存显示文本
    QString memory_text;
    {
        // 这里显示的是“显存占用率”（used/total），不是 GPU 核心利用率
        memory_text = QString("内存: %1GB/%2GB (%3%)")
            .arg(sys_stats.memory_used_bytes / 1024.0 / 1024.0 / 1024.0, 0, 'f', 1)
            .arg(sys_stats.memory_total_bytes / 1024.0 / 1024.0 / 1024.0, 0, 'f', 1)
            .arg(sys_stats.memory_usage_percent, 0, 'f', 0);
    }

    // 根据是否直播构建状态文本
    QString status_text;
    QString style;

    // 判断是否正在直播（统一使用stream_pusher_的推送状态）
    bool is_pushing = stream_pusher_ && stream_pusher_->is_pushing();

    if (is_pushing) {
        // 直播中：显示实际码率和FPS（从stream_pusher_获取）
        auto stats = stream_pusher_->get_stats();
        QString bitrate_fps_text = QString("码率: %1kb/s | FPS: %2 | ")
            .arg(static_cast<int>(stats.bandwidth_kbps))
            .arg(stats.video_fps, 0, 'f', 2);
        status_text = QString("%1CPU: %2% | %3")
            .arg(bitrate_fps_text)
            .arg(QString::number(sys_stats.cpu_usage_percent, 'f', 1))
            .arg(memory_text);
        style = "font-size: 12px; color: #cccccc;";
    } else {
        // 非直播：码率和FPS显示为0
        status_text = QString("码率: 0kb/s | FPS: 0.00 | CPU: %1% | %2")
            .arg(QString::number(sys_stats.cpu_usage_percent, 'f', 1))
            .arg(memory_text);
        style = "font-size: 12px; color: #888888;";
    }

    if (ui->label_techStats) {
        ui->label_techStats->setText(status_text);
        ui->label_techStats->setStyleSheet(style);
    }
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
        if (stageBtnMax_) { stageBtnMax_->setIcon(QIcon(":/images/Frame_recover@2x.png")); stageBtnMax_->setToolTip("还原"); }
        if (ui->pushButton_maximize) { ui->pushButton_maximize->setIcon(QIcon(":/images/Frame_recover@2x.png")); ui->pushButton_maximize->setToolTip("还原"); }
    } else {
        // restore
        stageContainer_->setGeometry(stage_normal_geometry_);
        stage_maximized_ = false;
        stageBtnRestore_->setVisible(false);
        if (stageBtnMax_) { stageBtnMax_->setIcon(QIcon(":/images/Frame_Max@2x.png")); stageBtnMax_->setToolTip("最大化"); }
        if (ui->pushButton_maximize) { ui->pushButton_maximize->setIcon(QIcon(":/images/Frame_Max@2x.png")); ui->pushButton_maximize->setToolTip("最大化"); }
    }
}

void MainWindow::restoreStage() {
    if (!stageContainer_) return;
    stageContainer_->setGeometry(stage_normal_geometry_);
    stage_maximized_ = false;
    stageBtnRestore_->setVisible(false);
    if (stageBtnMax_) { stageBtnMax_->setIcon(QIcon(":/images/Frame_Max@2x.png")); stageBtnMax_->setToolTip("最大化"); }
    if (ui->pushButton_maximize) { ui->pushButton_maximize->setIcon(QIcon(":/images/Frame_Max@2x.png")); ui->pushButton_maximize->setToolTip("最大化"); }
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

        // 设置摄像头列表
        auto camera_choices = video_engine_->get_available_camera_choices();
        dlg.set_available_cameras(camera_choices);

        // 获取当前摄像头配置
        std::string current_resolution = video_engine_->get_camera_resolution();
        int current_fps = video_engine_->get_camera_fps();
        bool current_mirror = video_engine_->get_camera_mirror();

        // 从 SceneItem 的 Transform 中读取镜像状态，以及获取原始设备ID
        auto current_transform = item->get_transform();
        current_mirror = current_transform.mirror;
        std::string current_device_id = item->get_device_id();  // 原始设备ID

        // 设置摄像头配置（使用当前的实际配置）
        dlg.set_camera_config(current_device_id, current_resolution, current_fps, current_mirror);

        if (dlg.exec() == QDialog::Accepted) {
            // 应用通用设置
            applySettingsPanelChanges(dlg);

            // 获取新的摄像头配置
            std::string new_resolution = dlg.get_camera_resolution();
            int new_fps = dlg.get_camera_fps();
            bool new_mirror = dlg.is_camera_mirror();

            LOG_INFO("摄像头设置更新 - 分辨率: " + new_resolution +
                     ", 帧率: " + std::to_string(new_fps) +
                     ", 镜像: " + (new_mirror ? "开启" : "关闭"));

            // 更新 video_engine 配置
            video_engine_->set_camera_resolution(new_resolution);
            video_engine_->set_camera_fps(new_fps);
            video_engine_->set_camera_mirror(new_mirror);

            // 更新镜像显示
            if (current_mirror != new_mirror) {
                auto transform = item->get_transform();
                transform.mirror = new_mirror;
                item->set_transform(transform);
            }

            // 如果分辨率或帧率改变了，提示用户
            if (new_resolution != current_resolution || new_fps != current_fps) {
                QMessageBox::information(this, "提示",
                    "分辨率或帧率已更改，需要重新添加摄像头才能生效。\n"
                    "如需应用这些更改，请删除当前摄像头后重新添加。");
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

    // 同步更新画布配置（分辨率改变时）
    if (canvas_config_.get_width() != new_v.width || canvas_config_.get_height() != new_v.height) {
        LOG_INFO(QString("分辨率改变: %1x%2 -> %3x%4")
            .arg(canvas_config_.get_width()).arg(canvas_config_.get_height())
            .arg(new_v.width).arg(new_v.height).toStdString());

        // 更新画布配置
        if (new_v.width > new_v.height) {
            // 横屏模式
            canvas_config_ = CanvasConfig(CanvasConfig::DisplayMode::LANDSCAPE_16_9);
        } else {
            // 竖屏模式
            canvas_config_ = CanvasConfig(CanvasConfig::DisplayMode::PORTRAIT_9_16);
        }
        // 手动设置为用户选择的分辨率
        // 注意：CanvasConfig 会根据 aspect ratio 自动计算，这里我们直接设置
        is_portrait_mode_ = (new_v.width < new_v.height);

        // 更新画布 widget
        if (canvas_widget_) {
            canvas_widget_->set_canvas_config(canvas_config_);
        }
    }

    // Apply audio settings
    auto new_a = dlg.get_audio_config();
    // 保留音频码率配置，允许用户在设置面板中修改
    // new_a.bitrate 从设置面板获取，使用用户设置的值

    const std::string mic_id = dlg.get_selected_microphone_id();
    const std::string speaker_id = dlg.get_selected_speaker_id();
    float mic_volume = dlg.get_microphone_volume();
    float speaker_volume = dlg.get_speaker_volume();

    // Apply camera mirror setting
    bool mirror = dlg.is_camera_mirror();
    if (video_engine_) {
        video_engine_->set_camera_mirror(mirror);
    }

    // Update camera SceneItem's Transform mirror setting
    if (scene_manager_ && scene_manager_->get_current_scene()) {
        auto scene = scene_manager_->get_current_scene();
        auto items = scene->get_all_scene_items();
        for (auto& item : items) {
            if (item && item->get_source() &&
                QString::fromStdString(item->get_source()->get_id()).startsWith("camera_")) {
                // Update the Transform with new mirror setting
                Transform tr = item->get_transform();
                tr.mirror = mirror;
                scene->set_transform(item, tr);
                break; // Only update the first camera item
            }
        }
    }

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

QString MainWindow::extract_source_name(std::shared_ptr<Source> source, std::shared_ptr<SceneItem> item) {
    if (!source) return "Unknown";

    // 优先使用 SceneItem 的 display_name
    if (item) {
        const std::string& dn = item->get_display_name();
        if (!dn.empty()) {
            return QString::fromStdString(dn);
        }
    }

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

    // 事件驱动时打印，频繁调用时注释掉避免阻塞
    // LOG_INFO("[DIAG] sync_scene_to_compositor: scene=" + scene->get_name() +
    //          ", items_count=" + std::to_string(items.size()));

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

        // 允许源部分超出画布边界，实现裁剪效果
        // 只对小于画布的源做边界限制，大于等于画布的源允许自由移动
        int canvas_width = canvas_config_.get_width();
        int canvas_height = canvas_config_.get_height();
        if (canvas_widget_) {
            canvas_width = canvas_widget_->get_canvas_config().get_width();
            canvas_height = canvas_widget_->get_canvas_config().get_height();
        }

        int final_x = tr.x;
        int final_y = tr.y;
        int final_w = w;
        int final_h = h;

        // 只有当源小于画布时才限制位置
        if (w < canvas_width) {
            final_x = (std::max)(0, (std::min)(tr.x, canvas_width - w));
        }
        if (h < canvas_height) {
            final_y = (std::max)(0, (std::min)(tr.y, canvas_height - h));
        }
        // 尺寸不做钳制，保留原始尺寸

        compositor_->update_layer_transform(sid, QRectF(final_x, final_y, final_w, final_h), tr.opacity);
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
    saveAudioVolumeSettings();
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
        if (microphone_enabled_) {
            ui->pushButton_mic->setIcon(QIcon(":/images/Voice-on@2x.png"));
            ui->pushButton_mic->setToolTip(QString::fromUtf8("麦克风 (点击静音)\n右键选择设备"));
        } else {
            // 静音状态：显示关闭录音图标
            ui->pushButton_mic->setIcon(QIcon(":/images/Voice-off@2x.png"));
            ui->pushButton_mic->setToolTip(QString::fromUtf8("麦克风 (已静音)\n点击取消静音"));
        }
    }

    if (ui->slider_mic && audio_engine_) {
        int volume = static_cast<int>(audio_engine_->get_microphone_volume() * 100);
        ui->slider_mic->setValue(volume);
        ui->slider_mic->setEnabled(microphone_enabled_);
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
    if (audio_engine_ && audio_engine_->get_audio_capturer()) {
        // 控制扬声器采集开关（而不是静音）
        audio_engine_->get_audio_capturer()->set_speaker_capture_enabled(speaker_enabled_);
    }
    update_speaker_ui();
    saveAudioVolumeSettings();
    LOG_INFO(std::string("Speaker capture ") + (speaker_enabled_ ? "enabled" : "disabled"));
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
        if (speaker_enabled_) {
            ui->pushButton_speaker->setIcon(QIcon(":/images/Volume@2x.png"));
            ui->pushButton_speaker->setToolTip(QString::fromUtf8("扬声器 (点击静音)\n右键选择设备"));
        } else {
            // 静音状态：显示静音图标
            ui->pushButton_speaker->setIcon(QIcon(":/images/Volume-mute@2x.png"));
            ui->pushButton_speaker->setToolTip(QString::fromUtf8("扬声器 (已静音)\n点击取消静音"));
        }
    }

    if (ui->slider_speaker && audio_engine_) {
        int volume = static_cast<int>(audio_engine_->get_speaker_volume() * 100);
        ui->slider_speaker->setValue(volume);
        ui->slider_speaker->setEnabled(speaker_enabled_);
        if (ui->label_speakerLevel) {
            ui->label_speakerLevel->setText(QString::number(volume) + "%");
        }
    }
}

void MainWindow::show_speaker_menu(const QPoint& pos) {
    if (!audio_engine_) return;

    QMenu menu(this);
    menu.setTitle("选择扬声器");

    auto devices = audio_engine_->get_available_speakers();
    auto current_id = audio_engine_->get_selected_speaker_id();

    for (const auto& device : devices) {
        QAction* action = menu.addAction(QString::fromUtf8(device.name.c_str()));
        action->setCheckable(true);
        action->setChecked(device.id == current_id);

        connect(action, &QAction::triggered, this, [this, device_id = device.id]() {
            if (audio_engine_->select_speaker(device_id)) {
                LOG_INFO("Selected speaker: " + device_id);
                // 更新扬声器音量UI
                update_speaker_ui();
            }
        });
    }

    menu.exec(pos);
}

// 推流控制方法实现

void MainWindow::on_streaming_started() {
    LOG_INFO("推流状态：已开始");
    streaming_start_time_ms_ = QDateTime::currentMSecsSinceEpoch();
    if (ui->label_liveDuration) ui->label_liveDuration->setText("00:00:00");
    // 直播时长显示用更高频刷新，避免偶尔“跳两秒”的观感（实际时长仍按系统时钟计算）
    if (live_duration_timer_) live_duration_timer_->start(200);

    // 启动系统信息更新定时器（每秒更新，包含码率/FPS）
    if (system_info_timer_) {
        system_info_timer_->start(1000); // 每秒更新一次（直播时）
        // 立即更新一次系统信息
        update_system_info();
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
    // 不停止 system_info_timer_，改为3秒间隔持续更新系统信息
    if (system_info_timer_) {
        system_info_timer_->start(3000); // 每3秒更新一次
        // 立即更新一次系统信息显示（非直播状态）
        update_system_info();
    }
    if (system_log_timer_) system_log_timer_->stop(); // 停止日志打印定时器
    streaming_start_time_ms_ = 0;
    if (ui->label_liveDuration) ui->label_liveDuration->setText("00:00:00");

    // 非直播状态也显示系统信息（使用统一格式，码率和FPS显示为0）
    update_system_info();
}

void MainWindow::on_streaming_error(const QString& error) {
    LOG_ERROR("推流错误: " + error.toStdString());

    // 停止推流
    if (encoder_bridge_ && encoder_bridge_->is_streaming()) {
        encoder_bridge_->stop_streaming();
    }
    if (stream_pusher_ && stream_pusher_->is_pushing()) {
        stream_pusher_->stop();
    }

    // 停止音频采集
    if (audio_engine_ && audio_engine_->is_capturing()) {
        audio_engine_->stop_capture();
        update_audio_status("已停止", "gray");
    }

    // 停止编码桥接器
    if (encoder_bridge_) {
        encoder_bridge_->stop();
    }

    // 更新UI状态
    if (ui->pushButton_startLive) {
        ui->pushButton_startLive->setText("开始直播");
        ui->pushButton_startLive->setStyleSheet("");
    }
    if (ui->label_status) {
        ui->label_status->setText("推流失败");
        ui->label_status->setStyleSheet("color: red; font-weight: bold;");
    }

    // 显示错误弹窗
    QMessageBox::warning(this, "推流错误", error);
}

// 画布配置管理方法实现
void MainWindow::set_canvas_config(const CanvasConfig& config) {
    LOG_INFO("========== set_canvas_config START ==========");
    LOG_INFO(QString("Config name: %1, Resolution: %2x%3")
        .arg(QString::fromStdString(config.get_name()))
        .arg(config.get_width())
        .arg(config.get_height()).toStdString());
    LOG_INFO(QString("canvas_widget_ exists: %1, encoder_ exists: %2, encoder_bridge_ exists: %3, canvasContainer_ exists: %4")
        .arg(canvas_widget_ ? "yes" : "no")
        .arg(encoder_ ? "yes" : "no")
        .arg(encoder_bridge_ ? "yes" : "no")
        .arg(canvasContainer_ ? "yes" : "no").toStdString());

    canvas_config_ = config;

    // 更新CanvasWidget
    if (canvas_widget_) {
        LOG_INFO("Updating canvas_widget_ config");
        canvas_widget_->set_canvas_config(config);
    }

    // 更新编码器配置（仅在非初始化阶段，即canvas_widget_已存在时）
    // 避免在setup_canvas_widget中重复初始化编码器
    if (encoder_ && encoder_bridge_ && canvasContainer_) {
        int width = config.get_width();
        int height = config.get_height();
        LOG_INFO(QString("Reinitializing encoder with resolution: %1x%2").arg(width).arg(height).toStdString());

        // 重新初始化视频编码器
        VideoEncoderConfig video_config;
        video_config.width = width;
        video_config.height = height;
        video_config.fps = 30;
        video_config.bitrate = is_portrait_mode_ ? 2000000 : 2500000;
        video_config.gop = 60;
        video_config.b_frames_enabled = false;

        encoder_->reinitialize_video_encoder(video_config);
        encoder_bridge_->set_resolution(width, height);
    } else {
        LOG_INFO("Skipping encoder reinitialization (initialization phase)");
    }

    LOG_INFO("Canvas config updated to: " + config.get_name());
    LOG_INFO("========== set_canvas_config END ==========");
}

const CanvasConfig& MainWindow::get_canvas_config() const {
    return canvas_config_;
}

// 设置为横屏模式 16:9 (1280x720)
void MainWindow::set_landscape_mode() {
    LOG_INFO("========== set_landscape_mode START ==========");
    if (!is_portrait_mode_) {
        LOG_INFO("Already in landscape mode, returning");
        return;
    }

    // 如果正在推流，禁止切换
    if (encoder_bridge_ && encoder_bridge_->is_streaming()) {
        LOG_INFO("Streaming in progress, cannot switch");
        QMessageBox::warning(this, "画布切换",
            "正在直播推流中，无法切换画布方向。\n"
            "请先停止直播后再切换。");
        return;
    }

    LOG_INFO("Switching to landscape mode (1280x720)");

    // 更新画布配置
    canvas_config_ = CanvasConfig::get_default();
    is_portrait_mode_ = false;

    // 应用新的画布配置
    apply_canvas_config_change();

    // 更新按钮文本
    if (ui->pushButton_toggleOrientation) {
        ui->pushButton_toggleOrientation->setText("横屏");
    }

    LOG_INFO("Switched to landscape mode: 1280x720");
    LOG_INFO("========== set_landscape_mode END ==========");
}

// 设置为竖屏模式 9:16 (720x1280)
void MainWindow::set_portrait_mode() {
    LOG_INFO("========== set_portrait_mode START ==========");
    if (is_portrait_mode_) {
        LOG_INFO("Already in portrait mode, returning");
        return;
    }

    // 如果正在推流，禁止切换
    if (encoder_bridge_ && encoder_bridge_->is_streaming()) {
        LOG_INFO("Streaming in progress, cannot switch");
        QMessageBox::warning(this, "画布切换",
            "正在直播推流中，无法切换画布方向。\n"
            "请先停止直播后再切换。");
        return;
    }

    LOG_INFO("Switching to portrait mode (720x1280)");

    // 更新画布配置
    canvas_config_ = CanvasConfig::get_portrait();
    is_portrait_mode_ = true;

    // 应用新的画布配置
    apply_canvas_config_change();

    // 更新按钮文本
    if (ui->pushButton_toggleOrientation) {
        ui->pushButton_toggleOrientation->setText("竖屏");
    }

    LOG_INFO("Switched to portrait mode: 720x1280");
    LOG_INFO("========== set_portrait_mode END ==========");
}

// 切换横竖屏
void MainWindow::toggle_canvas_orientation() {
    // 防止用户快速连点导致频繁重初始化（尤其是编码器），引发卡顿/崩溃风险
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (last_canvas_toggle_ms_ != 0 && (now_ms - last_canvas_toggle_ms_) < 300) {
        LOG_WARNING("Canvas toggle throttled (too frequent)");
        return;
    }
    last_canvas_toggle_ms_ = now_ms;

    if (canvas_config_changing_.load()) {
        LOG_WARNING("Canvas toggle ignored (config change in progress)");
        return;
    }

    if (is_portrait_mode_) {
        set_landscape_mode();
    } else {
        set_portrait_mode();
    }
}

// 应用服务器配置的画布方向（预留接口）
void MainWindow::apply_server_canvas_config(const QString& orientation) {
    LOG_INFO("========== apply_server_canvas_config START ==========");
    server_canvas_orientation_ = orientation.toLower();

    LOG_INFO("Applying server canvas config: " + server_canvas_orientation_.toStdString());
    LOG_INFO(QString("Current mode before apply: %1").arg(is_portrait_mode_ ? "portrait" : "landscape").toStdString());

    if (server_canvas_orientation_ == "portrait") {
        set_portrait_mode();
    } else {
        // 默认横屏
        set_landscape_mode();
    }
    LOG_INFO("========== apply_server_canvas_config END ==========");
}

// 辅助方法：应用画布配置变更
void MainWindow::apply_canvas_config_change() {
    LOG_INFO("========== apply_canvas_config_change START ==========");

    bool expected = false;
    if (!canvas_config_changing_.compare_exchange_strong(expected, true)) {
        LOG_WARNING("apply_canvas_config_change skipped (already in progress)");
        return;
    }
    const auto reset_flag = qScopeGuard([this] {
        canvas_config_changing_.store(false);
    });

    int width = canvas_config_.get_width();
    int height = canvas_config_.get_height();
    LOG_INFO(QString("Applying canvas config: %1x%2, portrait=%3")
        .arg(width).arg(height).arg(is_portrait_mode_).toStdString());

    // 更新 CanvasWidget
    if (canvas_widget_) {
        LOG_INFO("Updating CanvasWidget");
        canvas_widget_->set_canvas_config(canvas_config_);
    }

    // 更新 Compositor
    if (compositor_) {
        LOG_INFO("Updating Compositor");
        compositor_->set_canvas_size(width, height);
    }

    // 更新 EncoderBridge
    if (encoder_bridge_) {
        LOG_INFO("Updating EncoderBridge");
        encoder_bridge_->set_resolution(width, height);
    }

    // 更新 VideoEngine
    if (video_engine_) {
        LOG_INFO("Reinitializing VideoEngine");
        video_engine_->initialize(width, height, 30);
    }

    // 更新编码器配置（如果不在推流中）
    if (encoder_ && encoder_bridge_ && !encoder_bridge_->is_streaming()) {
        LOG_INFO("Reinitializing video encoder");
        VideoEncoderConfig video_config;
        video_config.width = width;
        video_config.height = height;
        video_config.fps = 30;
        video_config.bitrate = is_portrait_mode_ ? 2000000 : 2500000;  // 竖屏可以适当降低码率
        video_config.gop = 60;
        video_config.b_frames_enabled = false;

        encoder_->reinitialize_video_encoder(video_config);
        LOG_INFO("Video encoder reinitialized for " + std::string(is_portrait_mode_ ? "portrait" : "landscape") +
                 " mode: " + std::to_string(width) + "x" + std::to_string(height));
    } else {
        LOG_INFO("Skipping encoder reinitialization (streaming or not ready)");
    }

    // 调整场景项位置适配新比例
    LOG_INFO("Adjusting scene items");
    adjust_scene_items_for_canvas_change();

    // 特殊处理：如果有插播视频在播放，更新其 Compositor layer 为全屏
    // 插播视频的 source_id 以 "insert_video_" 开头
    if (current_insert_video_source_ && compositor_) {
        const std::string source_id = current_insert_video_source_->get_id();
        if (source_id.find("insert_video_") == 0 && compositor_->has_layer(source_id)) {
            QRectF fullscreen_rect(0, 0, width, height);
            compositor_->update_layer_transform(source_id, fullscreen_rect, 1.0f);
            LOG_INFO("Updated insert video layer transform for portrait mode: " +
                     std::to_string(width) + "x" + std::to_string(height));
        }
    }

    // 更新 UI
    LOG_INFO("Updating canvas orientation UI");
    update_canvas_orientation_ui();
    LOG_INFO("========== apply_canvas_config_change END ==========");
}

// 调整场景项位置以适配新的画布比例
void MainWindow::adjust_scene_items_for_canvas_change() {
    if (!scene_manager_ || !scene_manager_->get_current_scene()) {
        return;
    }

    auto scene = scene_manager_->get_current_scene();
    auto items = scene->get_all_scene_items();

    int canvas_w = canvas_config_.get_width();
    int canvas_h = canvas_config_.get_height();

    for (auto& item : items) {
        if (!item) continue;

        auto transform = item->get_transform();

        // 确保场景项在新画布范围内
        if (transform.x + transform.width > canvas_w) {
            transform.x = (std::max)(0, canvas_w - transform.width);
        }
        if (transform.y + transform.height > canvas_h) {
            transform.y = (std::max)(0, canvas_h - transform.height);
        }

        // 如果场景项超出画布，缩小它
        if (transform.width > canvas_w) {
            transform.width = canvas_w / 2;
        }
        if (transform.height > canvas_h) {
            transform.height = canvas_h / 2;
        }

        // 确保不超出边界
        transform.x = (std::min)(transform.x, canvas_w - transform.width);
        transform.y = (std::min)(transform.y, canvas_h - transform.height);

        scene->set_transform(item, transform);
    }

    // 同步到 compositor
    sync_scene_to_compositor();

    // 刷新画布
    if (canvas_widget_) {
        canvas_widget_->refresh();
    }

    LOG_INFO("Scene items adjusted for new canvas size: " +
             std::to_string(canvas_w) + "x" + std::to_string(canvas_h));
}

// 更新画布方向相关的 UI
void MainWindow::update_canvas_orientation_ui() {
    // 注意：不在 title/状态栏上显示横竖屏字样或分辨率
    LOG_INFO("Canvas orientation UI updated");

    // 调整画布容器大小以匹配新的宽高比
    update_stage_container_aspect_ratio();
}

// 根据当前画布方向调整舞台容器的宽高比
void MainWindow::update_stage_container_aspect_ratio() {
    if (!ui || !ui->liveArea) {
        return;
    }

    // 获取画布宽高比
    double canvas_aspect = canvas_config_.get_aspect_ratio();

    // 获取可用空间
    int available_width, available_height;
    if (canvasContainer_) {
        QRect containerRect = canvasContainer_->rect();
        available_width = containerRect.width();
        available_height = containerRect.height();
    } else if (stagePlaceholderWidget_) {
        QRect placeholderRect = stagePlaceholderWidget_->rect();
        available_width = placeholderRect.width();
        available_height = placeholderRect.height();
    } else {
        QRect liveAreaRect = ui->liveArea->rect();
        available_width = liveAreaRect.width();
        available_height = liveAreaRect.height();
    }

    // 计算适应可用空间的最大画布尺寸（保持宽高比）
    int target_width, target_height;

    // 计算按宽度限制的高度
    int height_by_width = static_cast<int>(available_width / canvas_aspect);
    // 计算按高度限制的宽度
    int width_by_height = static_cast<int>(available_height * canvas_aspect);

    if (height_by_width <= available_height) {
        // 按宽度限制
        target_width = available_width;
        target_height = height_by_width;
    } else {
        // 按高度限制
        target_width = width_by_height;
        target_height = available_height;
    }

    // 确保最小尺寸
    target_width = qMax(target_width, 320);
    target_height = qMax(target_height, 180);

    // 居中放置
    int x = (available_width - target_width) / 2;
    int y = (available_height - target_height) / 2;

    // 如果有 canvasContainer_（setup_canvas_widget 之后），使用它
    if (canvasContainer_ && canvas_widget_) {
        // 设置 canvas_widget_ 在容器中的位置和大小
        canvas_widget_->setGeometry(x, y, target_width, target_height);

        // 更新占位符位置
        repositionPlaceholderOverlays();

        LOG_INFO("Canvas widget resized for " + std::string(is_portrait_mode_ ? "portrait" : "landscape") +
                 " mode: " + std::to_string(target_width) + "x" + std::to_string(target_height) +
                 " (aspect: " + std::to_string(canvas_aspect) + ")");
        return;
    }

    // 如果有 stageContainer_（初始化阶段），使用它
    if (stageContainer_ && stagePlaceholderWidget_) {
        // 设置舞台容器几何位置
        stageContainer_->setGeometry(x, y, target_width, target_height);

        // 同步更新画布控件
        if (canvas_widget_) {
            canvas_widget_->setGeometry(0, 0, target_width, target_height);
        }

        // 更新占位符按钮位置
        if (stageAddButton_) {
            stageAddButton_->setGeometry(stageContainer_->rect());
        }
    }

    LOG_INFO("Stage container resized for " + std::string(is_portrait_mode_ ? "portrait" : "landscape") +
             " mode: " + std::to_string(target_width) + "x" + std::to_string(target_height) +
             " (aspect: " + std::to_string(canvas_aspect) + ")");
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    
    if (event->type() == QEvent::WindowStateChange) {
        if (ui->pushButton_maximize) {
            if (isMaximized()) {
                ui->pushButton_maximize->setIcon(QIcon(":/images/Frame_recover@2x.png"));
                ui->pushButton_maximize->setProperty("maximized", true);
            } else {
                ui->pushButton_maximize->setIcon(QIcon(":/images/Frame_Max@2x.png"));
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
            if (system_info_timer_) system_info_timer_->stop();
            if (encoding_timer_) encoding_timer_->stop();
            
            // 停止推流
            if (encoder_bridge_ && encoder_bridge_->is_streaming()) {
                encoder_bridge_->stop_streaming();
            }

            // 保存音量设置
            saveAudioVolumeSettings();

            // 保存场景配置
            save_scenes_config();

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
    // 先设置标志，防止初始化时误触发显示窗口
    tray_icon_initializing_ = true;
    connect(system_tray_icon_, &QSystemTrayIcon::activated,
            this, &MainWindow::onTrayIconActivated);
    
    // 显示托盘图标
    system_tray_icon_->show();
    
    // 延迟重置标志，确保托盘图标完全初始化后再响应用户点击
    QTimer::singleShot(500, this, [this]() {
        tray_icon_initializing_ = false;
        LOG_INFO("Tray icon initialized, ready to respond to user clicks");
    });

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
    // 忽略初始化期间的托盘激活事件，防止窗口闪烁
    if (tray_icon_initializing_) {
        LOG_DEBUG("Ignoring tray icon activation during initialization");
        return;
    }
    
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
    LOG_INFO("User requested to exit live room - emit request_return_to_live_list signal");

    // 保存场景配置（返回直播列表时也保存）
    save_scenes_config();

    // 停止所有采集源（摄像头、屏幕共享等）
    stop_all_capture_sources();

    // 停止音频采集
    if (audio_engine_) {
        audio_engine_->stop_capture();
    }

    // 发射信号通知 main.cpp 用户想要返回直播列表
    emit request_return_to_live_list();
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

void MainWindow::saveAudioVolumeSettings() {
    QSettings settings("LiveAssistant", "Settings");
    if (audio_engine_) {
        settings.setValue("microphoneVolume", audio_engine_->get_microphone_volume());
        settings.setValue("speakerVolume", audio_engine_->get_speaker_volume());
        settings.setValue("microphoneEnabled", microphone_enabled_);
        settings.setValue("speakerEnabled", speaker_enabled_);
        LOG_INFO("Saved audio volume settings");
    }
}

void MainWindow::loadAudioVolumeSettings() {
    QSettings settings("LiveAssistant", "Settings");

    // 检查是否有保存的音量配置
    bool hasSavedMicVolume = settings.contains("microphoneVolume");
    bool hasSavedSpeakerVolume = settings.contains("speakerVolume");

    float micVolume;
    float speakerVolume;

    if (hasSavedMicVolume || hasSavedSpeakerVolume) {
        // 有保存的配置，使用保存的值
        micVolume = settings.value("microphoneVolume", 0.4f).toFloat();
        speakerVolume = settings.value("speakerVolume", 0.4f).toFloat();
        LOG_INFO("Using saved volume settings");
    } else {
        // 没有保存的配置，使用系统当前的音量设置
        if (audio_engine_) {
            micVolume = audio_engine_->get_microphone_volume();
            speakerVolume = audio_engine_->get_speaker_volume();
        } else {
            // 默认40%
            micVolume = 0.4f;
            speakerVolume = 0.4f;
        }
        LOG_INFO("No saved volume settings, using system current volume");
    }

    // 加载静音状态（默认开启）
    microphone_enabled_ = settings.value("microphoneEnabled", true).toBool();
    speaker_enabled_ = settings.value("speakerEnabled", true).toBool();

    // 应用到音频引擎
    if (audio_engine_) {
        audio_engine_->set_microphone_volume(micVolume);
        audio_engine_->set_speaker_volume(speakerVolume);
        audio_engine_->set_microphone_mute(!microphone_enabled_);
        audio_engine_->set_speaker_mute(!speaker_enabled_);
    }

    LOG_INFO("Loaded audio volume settings: mic=" + std::to_string(static_cast<int>(micVolume * 100)) +
             "%, speaker=" + std::to_string(static_cast<int>(speakerVolume * 100)) + "%");
}

void MainWindow::playVolumeFeedbackSound() {
    // 静音处理，不再播放音量反馈声音
}

} // namespace live_assistant
