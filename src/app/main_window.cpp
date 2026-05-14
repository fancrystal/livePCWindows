#include "app/main_window.h"
#include "app/camera_settings.h"
#include "app/screen_share.h"
#include "app/screen_capture_selector.h"
#include "app/settings_dialog.h"
#include "app/exit_dialog.h"
#include "app/insert_video_widget.h"
#include "app/insert_file_manager.h"
#include "app/add_material_dialog.h"
#include "app/share_settings_dialog.h"
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
#include <set>
#include "ui_main_window.h"
#include "scene_manager/scene_manager.h"
#include "scene_manager/source_factory.h"
#include "scene_manager/canvas.h"
#include "scene_manager/compositor.h"
#include "scene_manager/gpu_compositor.h"
#include "scene_manager/gpu_color_converter.h"
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
#include <QResizeEvent>
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

namespace {

Transform make_centered_fit_transform(
    int canvas_w,
    int canvas_h,
    int src_w,
    int src_h,
    double max_canvas_width_ratio,
    double max_canvas_height_ratio,
    bool mirror = false) {
    canvas_w = (std::max)(canvas_w, 1);
    canvas_h = (std::max)(canvas_h, 1);
    src_w = (std::max)(src_w, 1);
    src_h = (std::max)(src_h, 1);

    const int max_w = (std::max)(1, static_cast<int>(canvas_w * max_canvas_width_ratio));
    const int max_h = (std::max)(1, static_cast<int>(canvas_h * max_canvas_height_ratio));
    const double scale = (std::min)(
        static_cast<double>(max_w) / src_w,
        static_cast<double>(max_h) / src_h);

    const int target_w = (std::max)(1, static_cast<int>(src_w * scale));
    const int target_h = (std::max)(1, static_cast<int>(src_h * scale));
    const int x = (canvas_w - target_w) / 2;
    const int y = (canvas_h - target_h) / 2;
    return Transform(x, y, target_w, target_h, 0.0f, 1.0f, mirror);
}

}

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow) {
    ui->setupUi(this);

    app_settings_.load();
    canvas_config_ = app_settings_.canvas;
    is_portrait_mode_ = (canvas_config_.get_width() < canvas_config_.get_height());

    settings_applier_ = std::make_unique<SettingsApplier>(this);
    app_settings_.add_observer(settings_applier_.get());

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
        logoLbl->setFixedSize(32, 32);  
        QPixmap iconPix(":/images/logo_new.png");
        if (!iconPix.isNull()) {
            QPixmap scaledPix = iconPix.scaled(32, 32, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            logoLbl->setPixmap(scaledPix);
        }
        tlay->addWidget(logoLbl);

        // gradient text pixmap for main title
        QString mainText = QString::fromUtf8("灵犀");
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
        QLabel* suffix = new QLabel(QString::fromUtf8("·视频云"), titleContainer);
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
    setAttribute(Qt::WA_TranslucentBackground, false);
    setAttribute(Qt::WA_NoSystemBackground, false);
    setAutoFillBackground(true);
    setAttribute(Qt::WA_StyledBackground, true);
    if (ui->centralWidget) {
        ui->centralWidget->setAttribute(Qt::WA_StyledBackground, true);
        ui->centralWidget->setAutoFillBackground(true);
    }
    if (ui->topBar) {
        ui->topBar->setAttribute(Qt::WA_StyledBackground, true);
    }
    if (ui->liveRoomInfoBar) {
        ui->liveRoomInfoBar->setAttribute(Qt::WA_StyledBackground, true);
    }
    if (ui->liveArea) {
        ui->liveArea->setAttribute(Qt::WA_StyledBackground, true);
    }
    if (ui->rightSidebar) {
        ui->rightSidebar->setAttribute(Qt::WA_StyledBackground, true);
    }
    
    // Install event filter on topBar to enable window dragging
    if (ui->topBar) {
        ui->topBar->installEventFilter(this);
        ui->topBar->setAttribute(Qt::WA_Hover, true);
    }
    if (ui->label_liveRoomTitle) {
        live_title_label_ = ui->label_liveRoomTitle;
        ui->label_liveRoomTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
        ui->label_liveRoomTitle->setToolTip(ui->label_liveRoomTitle->text());
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

    loadExitPreference();

    QTimer::singleShot(1000, this, [this]() {
        setupSystemTray();
        LOG_INFO("System tray initialized after delay");
    });

    setupNetworkConnections();

    update_status("Ready");

    setupDpiChangeHandling();

    
    initWebEngineUI();

    LOG_INFO("MainWindow created");
}

void MainWindow::initWebEngineUI()
{
    if (!ui->webEngineView_chat || !ui->webEngineView_product) {
        return;
    }

    ui->webEngineView_chat->setAttribute(Qt::WA_NativeWindow, false);
    ui->webEngineView_chat->setAttribute(Qt::WA_DontCreateNativeAncestors, true);
    ui->webEngineView_product->setAttribute(Qt::WA_NativeWindow, false);
    ui->webEngineView_product->setAttribute(Qt::WA_DontCreateNativeAncestors, true);

    ui->webEngineView_chat->setAttribute(Qt::WA_AcceptTouchEvents, true);
    ui->webEngineView_product->setAttribute(Qt::WA_AcceptTouchEvents, true);

    ui->webEngineView_chat->settings()->setAttribute(QWebEngineSettings::PluginsEnabled, true);
    ui->webEngineView_product->settings()->setAttribute(QWebEngineSettings::PluginsEnabled, true);

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
    LOG_INFO("initWebEngineViews called - WebView loading disabled for debugging");
    return;

    /*
    if (!ui->webEngineView_chat || !ui->webEngineView_product) {
        LOG_WARNING("WebEngineView controls not found");
        return;
    }

    if (!ui->webEngineView_chat->url().isEmpty() || !ui->webEngineView_product->url().isEmpty()) {
        LOG_INFO("WebEngineViews already initialized, skipping...");
        return;
    }

    QString token = token_;
    if (token.startsWith("Bearer ")) {
        token = token.mid(7);
    }

    LOG_INFO("Initializing WebEngineViews with domain: " + domain_.toStdString());

    auto initCustomWebEngine = [this, token](QWebEngineView* webView, const QString& urlStr) {
        CustomWebEngineView* customWebView = dynamic_cast<CustomWebEngineView*>(webView);
        if (customWebView) {
            customWebView->setAuthorizationToken(token, domain_);
            customWebView->setCustomUrl(QUrl(urlStr));
        }
    };

    if (!current_live_item_.liveId.isEmpty()) {
        QString chatUrl = "https://" + domain_ + "/livesaas/liveStream/livedetails?roomInfoId=" + current_live_item_.liveId + "&embed=onlyInfo&tab=chat";
        LOG_INFO("Chat WebView URL: " + chatUrl.toStdString());
        initCustomWebEngine(ui->webEngineView_chat, chatUrl);
    }

    if (!current_live_item_.liveId.isEmpty()) {
        QString goodsUrl = "https://" + domain_ + "/livesaas/liveStream/livedetails?roomInfoId=" + current_live_item_.liveId + "&embed=onlyInfo&tab=goods";
        LOG_INFO("Product WebView URL: " + goodsUrl.toStdString());
        initCustomWebEngine(ui->webEngineView_product, goodsUrl);
    }
    */
}

MainWindow::~MainWindow() {
    LOG_INFO("MainWindow destroyed");

    // ── 1. 停止所有定时器，防止析构过程中触发回调 ──────────────────────
    if (encoding_timer_)      encoding_timer_->stop();
    if (preview_timer_)       preview_timer_->stop();
    if (live_duration_timer_) live_duration_timer_->stop();
    if (system_log_timer_)    system_log_timer_->stop();
    if (system_info_timer_)   system_info_timer_->stop();
    if (dpi_relayout_timer_)  dpi_relayout_timer_->stop();
    if (insert_video_timer_)  insert_video_timer_->stop();

    // ── 2. 清空 encoder_bridge_ 对 CanvasRenderer 的引用 ─────────────
    // encoder_bridge_ 持有 CanvasRenderer* (非拥有裸指针)；显式清空避免悬空指针
    // CanvasRenderer 由 canvas_widget_->renderer_ (unique_ptr) 管理，稍后随 Qt child 一起析构
    if (encoder_bridge_) {
        encoder_bridge_->set_canvas_renderer(nullptr, nullptr);
    }

    // ── 3. 清空 canvas_widget_ 对共享资源的引用 ──────────────────────
    // canvas_widget_ 是 Qt child（在 ~QObject() 才释放），但 shared_ptr 持有的模块
    // 会在 C++ 成员析构阶段提前销毁；提前解除引用避免 canvas_widget_ 回调悬空指针
    if (canvas_widget_) {
        canvas_widget_->set_compositor(nullptr);
        canvas_widget_->set_video_engine(nullptr);
    }

    // ── 4. 清理系统托盘 ──────────────────────────────────────────────
    cleanupSystemTray();

    // ── 5. 删除 Qt UI（会移除其管理的所有 Widget children） ─────────
    delete ui;

    // 注意：C++ 成员（shared_ptr）随后按逆声明顺序自动析构
    // encoder_bridge_ → gpu_color_converter_ → gpu_compositor_ → compositor_
    // → capture_manager_ → stream_pusher_ → encoder_ → audio_engine_
    // → video_engine_ → scene_manager_
    // canvas_widget_（Qt child）最后在 ~QObject() 中析构，届时上述模块已全部释放
}

void MainWindow::setupDpiChangeHandling() {
    last_normal_window_size_ = size();

    dpi_relayout_timer_ = new QTimer(this);
    dpi_relayout_timer_->setSingleShot(true);
    connect(dpi_relayout_timer_, &QTimer::timeout, this, [this]() {
        if (ui) {
            if (ui->centralWidget) {
                ui->centralWidget->updateGeometry();
                ui->centralWidget->update();
            }
            if (ui->liveArea) {
                ui->liveArea->updateGeometry();
                ui->liveArea->layout()->activate();
                ui->liveArea->update();
            }
            if (ui->rightSidebar) {
                ui->rightSidebar->updateGeometry();
                ui->rightSidebar->update();
            }
        }
        if (canvasContainer_) {
            canvasContainer_->updateGeometry();
            canvasContainer_->update();
        }
        if (canvas_widget_) {
            canvas_widget_->updateGeometry();
            canvas_widget_->update();
        }

        update_stage_container_aspect_ratio();
        repositionPlaceholderOverlays();
        updateGeometry();
        update();
    });

    QTimer::singleShot(0, this, [this]() {
        QWindow* handle = windowHandle();
        if (!handle) {
            return;
        }

        connect(handle, &QWindow::screenChanged, this, [this](QScreen* screen) {
            if (screen) {
                LOG_INFO("MainWindow screen changed, DPI: " + std::to_string(screen->logicalDotsPerInch()));
            }
            attachScreenDpiHandler(screen);
            scheduleDpiRelayout(true);
        });

        attachScreenDpiHandler(handle->screen());
    });
}

void MainWindow::attachScreenDpiHandler(QScreen* screen) {
    if (current_screen_dpi_connection_) {
        disconnect(current_screen_dpi_connection_);
        current_screen_dpi_connection_ = QMetaObject::Connection();
    }

    if (!screen) {
        return;
    }

    current_screen_dpi_connection_ =
        connect(screen, &QScreen::logicalDotsPerInchChanged, this, [this](qreal dpi) {
            LOG_INFO("MainWindow DPI changed to: " + std::to_string(dpi));
            scheduleDpiRelayout(true);
        });
}

void MainWindow::scheduleDpiRelayout(bool preserve_window_size) {
    if (preserve_window_size && !isMaximized() && !isMinimized() && last_normal_window_size_.isValid()) {
        const QSize current_size = size();
        const int width_delta = std::abs(current_size.width() - last_normal_window_size_.width());
        const int height_delta = std::abs(current_size.height() - last_normal_window_size_.height());

        // A cross-monitor DPI transition can briefly reinterpret frameless-window
        // geometry in physical pixels. Put the widget back to the last user-sized
        // logical dimensions before recomputing the stage.
        if (width_delta > 2 || height_delta > 2) {
            resize(last_normal_window_size_);
        }
    }

    if (dpi_relayout_timer_) {
        dpi_relayout_timer_->start(0);
    }
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

     if (ui) {
        if (ui->comboBox_scenes) {
            ui->comboBox_scenes->setVisible(true);
        }
        if (ui->pushButton_addScene) {
            ui->pushButton_addScene->setVisible(true);
            connect(ui->pushButton_addScene, &QPushButton::clicked, this, &MainWindow::on_add_scene_clicked);
        }
    }

    create_scene_buttons();
}

void MainWindow::build_scene_selector() {
    if (!scene_manager_) return;

    if (ui && ui->comboBox_scenes) {
        ui->comboBox_scenes->setVisible(true);

        ui->comboBox_scenes->blockSignals(true);

        ui->comboBox_scenes->clear();

        auto scene_names = scene_manager_->get_scene_names();
        for (const auto& name : scene_names) {
            ui->comboBox_scenes->addItem(QString::fromStdString(name));
        }

        auto current_scene = scene_manager_->get_current_scene();
        if (current_scene) {
            int index = ui->comboBox_scenes->findText(QString::fromStdString(current_scene->get_name()));
            if (index >= 0) {
                ui->comboBox_scenes->setCurrentIndex(index);
            }
        }

        static bool signal_connected = false;
        if (!signal_connected) {
            connect(ui->comboBox_scenes, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, &MainWindow::on_scene_selected);
            signal_connected = true;
        }

        ui->comboBox_scenes->blockSignals(false);
    }
}

void MainWindow::on_scene_selected(int index) {
    if (!scene_manager_ || !ui || !ui->comboBox_scenes) return;

    QString scene_name = ui->comboBox_scenes->itemText(index);
    if (scene_name.isEmpty()) return;

    // [DIAG] 场景切换诊断：记录切换时插播视频状态
    std::string diag_insert_id = current_insert_video_source_
        ? current_insert_video_source_->get_id()
        : std::string("null");
    LOG_INFO("[DIAG][SCENE_SWITCH] Switching to scene: " + scene_name.toStdString()
             + " | is_insert_video_playing=" + std::to_string(is_insert_video_playing_)
             + " | insert_source_id=" + diag_insert_id);

    scene_manager_->set_current_scene(scene_name.toStdString());

    auto current = scene_manager_->get_current_scene();
    if (!current) {
        LOG_ERROR("[MainWindow] on_scene_selected: get_current_scene() returned null after set, index may be invalid");
        return;
    }

    // 切换场景时：若当前有插播视频在播放，且它不属于新场景，则暂停它
    // 暂停（不是停止）：保留断点位置，切回原场景时可以继续
    // 同时 detach encoder_bridge，新场景画面不再叠加旧场景的插播视频
    if (is_insert_video_playing_ && current_insert_video_source_) {
        bool insert_in_new_scene = is_source_in_current_scene(current_insert_video_source_->get_id());
        if (!insert_in_new_scene) {
            LOG_INFO("[SCENE_SWITCH] Pausing insert video (belongs to old scene): " +
                     current_insert_video_source_->get_id());
            pauseCurrentInsertVideo();
        }
    }

    if (canvas_widget_) {
        canvas_widget_->set_current_scene(scene_name.toStdString());
    }

    if (video_engine_) {
        video_engine_->set_current_scene(current);
    }

    build_scene_list();

    sync_scene_to_compositor();

    LOG_INFO("Switched to scene: " + scene_name.toStdString());
}

void MainWindow::on_add_scene_clicked() {
    if (!scene_manager_) return;

    bool ok = false;
    QString new_name = QInputDialog::getText(this, "添加场景", "请输入新场景名称:",
                                              QLineEdit::Normal, "新场景", &ok);
    if (!ok || new_name.isEmpty()) return;

    auto scene_names = scene_manager_->get_scene_names();
    for (const auto& name : scene_names) {
        if (name == new_name.toStdString()) {
            QMessageBox::warning(this, "错误", "场景名称已存在！");
            return;
        }
    }

    if (scene_manager_->create_scene(new_name.toStdString()) == ErrorCode::SUCCESS) {
        scene_manager_->set_current_scene(new_name.toStdString());

        if (canvas_widget_) {
            canvas_widget_->set_current_scene(new_name.toStdString());
        }

        build_scene_selector();

        build_scene_list();

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

    int ret = QMessageBox::question(this, "删除场景",
        QString("确定要删除场景 \"%1\" 吗？").arg(QString::fromStdString(current_scene->get_name())),
        QMessageBox::Yes | QMessageBox::No);

    if (ret != QMessageBox::Yes) return;

    QString scene_name = QString::fromStdString(current_scene->get_name());


    if (scene_manager_->remove_scene(scene_name.toStdString()) == ErrorCode::SUCCESS) {

        build_scene_selector();


        build_scene_list();


        sync_scene_to_compositor();

        LOG_INFO("Removed scene: " + scene_name.toStdString());
    }
}

void MainWindow::on_rename_scene_clicked() {
    if (!scene_manager_) return;

    auto current_scene = scene_manager_->get_current_scene();
    if (!current_scene) return;


    bool ok = false;
    QString new_name = QInputDialog::getText(this, "重命名场景", "请输入新场景名称:",
                                              QLineEdit::Normal,
                                              QString::fromStdString(current_scene->get_name()), &ok);
    if (!ok || new_name.isEmpty()) return;


    auto scene_names = scene_manager_->get_scene_names();
    for (const auto& name : scene_names) {
        if (name == new_name.toStdString() && name != current_scene->get_name()) {
            QMessageBox::warning(this, "错误", "场景名称已存在！");
            return;
        }
    }


    if (current_scene->set_name(new_name.toStdString()) == ErrorCode::SUCCESS) {

        build_scene_selector();

        LOG_INFO("Renamed scene to: " + new_name.toStdString());
    }
}

void MainWindow::create_scene_buttons() {

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


    QString config_dir_local = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir dir_local(config_dir_local);
    if (!dir_local.exists()) {
        dir_local.mkpath(config_dir_local);
    }


    QString config_dir_roaming = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir dir_roaming(config_dir_roaming);
    if (!dir_roaming.exists()) {
        dir_roaming.mkpath(config_dir_roaming);
    }


    QString config_file_local;
    QString config_file_roaming;
    if (!live_id_.isEmpty()) {
        config_file_local = config_dir_local + "/scenes_" + live_id_ + ".json";
        config_file_roaming = config_dir_roaming + "/scenes_" + live_id_ + ".json";
    } else {
        config_file_local = config_dir_local + "/scenes_default.json";
        config_file_roaming = config_dir_roaming + "/scenes_default.json";
    }


    QJsonArray scenes_array = scene_manager_->serialize();


    QJsonObject config_obj;
    config_obj["scenes"] = scenes_array;
    config_obj["is_portrait"] = is_portrait_mode_;

    QJsonDocument doc(config_obj);
    QByteArray json_data = doc.toJson(QJsonDocument::Indented);


    QFile file_local(config_file_local);
    if (file_local.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file_local.write(json_data);
        file_local.close();
        LOG_INFO("Scenes config saved to: " + config_file_local.toStdString());
    } else {
        LOG_ERROR("Failed to save scenes config to: " + config_file_local.toStdString());
    }


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


    QString config_dir_local = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir dir_local(config_dir_local);
    if (!dir_local.exists()) {
        dir_local.mkpath(config_dir_local);
    }


    QString config_dir_roaming = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir dir_roaming(config_dir_roaming);
    if (!dir_roaming.exists()) {
        dir_roaming.mkpath(config_dir_roaming);
    }


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


    QFile file(config_file);
    if (!file.exists()) {

        QString old_file_local = config_dir_local + "/scenes.json";
        LOG_INFO("Checking legacy file (Local): " + old_file_local.toStdString());
        QFile old_file_check1(old_file_local);
        if (old_file_check1.exists()) {
            config_file = old_file_local;
            LOG_INFO("Using legacy scenes config file (Local)");
        } else {

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
    bool has_saved_portrait_mode = false;


    if (doc.isObject()) {
        QJsonObject config_obj = doc.object();
        if (config_obj.contains("scenes")) {
            scenes_array = config_obj["scenes"].toArray();
            if (config_obj.contains("is_portrait")) {
                has_saved_portrait_mode = true;
                loaded_portrait_mode = config_obj["is_portrait"].toBool(false);
                LOG_INFO("Loaded portrait mode from config: " + std::to_string(loaded_portrait_mode));
            }
        } else {

            scenes_array = config_obj.toVariantMap().value("scenes").toJsonArray();
        }
    } else if (doc.isArray()) {
        scenes_array = doc.array();
    } else {
        LOG_ERROR("Scenes config is not a valid format");
        return;
    }


    if (scene_manager_->deserialize(scenes_array) == ErrorCode::SUCCESS) {
        if (!server_canvas_orientation_.isEmpty()) {
            const bool server_portrait_mode = (server_canvas_orientation_ == "portrait");
            LOG_INFO(QString("Server canvas orientation overrides saved scene orientation: %1")
                .arg(server_canvas_orientation_).toStdString());
            if (server_portrait_mode != is_portrait_mode_) {
                if (server_portrait_mode) {
                    set_portrait_mode();
                } else {
                    set_landscape_mode();
                }
            }
        } else if (has_saved_portrait_mode && loaded_portrait_mode != is_portrait_mode_) {
            LOG_INFO("Restoring portrait mode: " + std::to_string(loaded_portrait_mode));
            if (loaded_portrait_mode) {
                set_portrait_mode();
            } else {
                set_landscape_mode();
            }
        }


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


    auto scene_names = scene_manager_->get_scene_names();
    LOG_INFO("Restoring capture sources for " + std::to_string(scene_names.size()) + " scenes");

    for (const auto& scene_name : scene_names) {
        auto items = scene_manager_->get_scene_items(scene_name);
        LOG_INFO("Scene '" + scene_name + "' has " + std::to_string(items.size()) + " items");

        for (const auto& item : items) {
            if (!item) continue;

            const std::string source_id = item->get_source_id();
            QString qsource_id = QString::fromStdString(source_id);


            bool is_camera = qsource_id.startsWith("camera_");
            bool is_screen = qsource_id.startsWith("capture_");

            if (!is_camera && !is_screen) continue;


            if (capture_manager_->has_source(source_id)) {
                LOG_INFO("Capture source already exists: " + source_id);
                continue;
            }


            std::string device_id = item->get_device_id();
            const auto& params = item->get_source_params();

            if (device_id.empty()) {

                if (is_camera) {
                    device_id = qsource_id.mid(7).toStdString();
                } else if (is_screen) {
                    device_id = qsource_id.mid(8).toStdString();
                }
            }

            LOG_INFO("Restoring capture source: " + source_id + ", device_id: " + device_id);


            CaptureConfig cfg;
            if (is_camera) {
                cfg.type = CaptureConfig::TargetType::CAMERA;
                cfg.target_id = device_id;


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
                    cfg.capture_mode = CaptureMode::FFMPEG;
                }
                if (params.count("mirror")) {
                    cfg.mirror = (params.at("mirror") == "true");
                }

                LOG_INFO("恢复摄像头配置: resolution=" + cfg.resolution_string() +
                         ", fps=" + std::to_string(cfg.fps) +
                         ", pixel_format=" + pixel_format_to_string(cfg.pixel_format) +
                         ", capture_mode=" + capture_mode_to_string(cfg.capture_mode));
            } else if (is_screen) {

                bool is_screen_mode = true;


                cfg.type = CaptureConfig::TargetType::SCREEN;
                cfg.target_id = device_id;
                cfg.fps = 15;
                if (params.count("fps")) {
                    cfg.fps = std::stoi(params.at("fps"));
                }
            }


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



            connect(src.get(), &ICaptureSource::frameReady, this, [this, source_id](const CaptureFrame& frame) {

                if (compositor_ && !frame.image.isNull() && is_source_in_current_scene(source_id)) {
                    if (!compositor_->has_layer(source_id)) {
                        compositor_->add_layer(source_id);


                        if (auto item = find_current_scene_item(source_id)) {
                            compositor_->set_layer_order(source_id, item->get_order());

                            const auto& tr = item->get_transform();
                            int w = tr.width > 0 ? tr.width : canvas_config_.get_width();
                            int h = tr.height > 0 ? tr.height : canvas_config_.get_height();
                            compositor_->update_layer_transform(source_id,
                                QRectF(tr.x, tr.y, w, h), tr.opacity);
                            compositor_->set_layer_visible(source_id, item->is_visible());
                        }
                    }
                    compositor_->updateLayerImage(QString::fromStdString(source_id), frame.image);
                }


                if (scene_manager_) {

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

            // WGC 初始化失败（如安全软件触发 E_ACCESSDENIED）时弹出用户提示
            connect(src.get(), &ICaptureSource::captureError, this,
                    [this](const QString& /*source_id*/, const QString& error_message) {
                QMessageBox::warning(this, "屏幕共享失败", error_message);
            }, Qt::QueuedConnection);

            if (capture_manager_) {
                capture_manager_->add_source(source_id, src);
            }


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


    if (is_camera_preview_) {
        stop_camera_preview();
    }


    if (scene_manager_) {
        scene_manager_->cleanup_all_sources();
    }


    if (!capture_manager_) {
        LOG_INFO("CaptureManager not initialized, nothing to stop");
        LOG_INFO("========== stop_all_capture_sources END ==========");
        return;
    }


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


    listWidget_sceneItems_->blockSignals(true);
    listWidget_sceneItems_->clear();

    auto scene = scene_manager_->get_current_scene();
    auto scene_items = scene->get_all_scene_items();


    std::vector<std::shared_ptr<SceneItem>> camera_items;
    std::vector<std::shared_ptr<SceneItem>> other_items;
    
    for (const auto& item : scene_items) {
        if (QString::fromStdString(item->get_source_id()).startsWith("camera_")) {
            camera_items.push_back(item);
        } else {
            other_items.push_back(item);
        }
    }
    

    for (const auto& item : camera_items) {
        auto* lw_item = new QListWidgetItem(listWidget_sceneItems_);
        lw_item->setSizeHint(QSize(240, 34));
        lw_item->setData(Qt::UserRole, QString::fromStdString(item->get_source_id()));

        QString display_name = extract_source_name(item->get_source(), item);
        auto* row = new SceneItemRow(item, display_name, listWidget_sceneItems_);

        const int row_index = listWidget_sceneItems_->row(lw_item);
        row->set_move_up_enabled(false);

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



        listWidget_sceneItems_->setItemWidget(lw_item, row);
    }
    

    const int total = static_cast<int>(other_items.size());
    for (int i = total - 1; i >= 0; --i) {
        auto item = other_items[i];

        auto* lw_item = new QListWidgetItem(listWidget_sceneItems_);
        lw_item->setSizeHint(QSize(240, 34));
        lw_item->setData(Qt::UserRole, QString::fromStdString(item->get_source_id()));

        QString display_name = extract_source_name(item->get_source(), item);
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

            auto* target_item = listWidget_sceneItems_->item(r - 1);
            if (!target_item) return;
            QString target_sid = target_item->data(Qt::UserRole).toString();
            if (target_sid.startsWith("camera_")) {
                return;
            }
            listWidget_sceneItems_->model()->moveRow(QModelIndex(), r, QModelIndex(), r - 1);
        });

        listWidget_sceneItems_->setItemWidget(lw_item, row);
    }


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

                if (!sid.startsWith("camera_")) {
                    it->set_order(order);
                } else {

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


        domain_ = "b-test.lxi-tech.com";
        LOG_INFO("Extracted domain: " + domain_.toStdString());
    }

    LOG_INFO(QString("Credentials set - userId: %1, liveUrl: %2, socketUrl: %3")
        .arg(userId).arg(liveurl).arg(socketUrl).toStdString());
}

void MainWindow::setLiveItem(const LiveItem& liveItem) {
    LOG_INFO("========== setLiveItem START ==========");


    bool live_id_changed = !liveItem.liveId.isEmpty() && live_id_ != liveItem.liveId;
    if (live_id_changed) {
        LOG_INFO(QString("Live ID changed: %1 -> %2").arg(live_id_).arg(liveItem.liveId).toStdString());
    }

    current_live_item_ = liveItem;


    if (live_title_label_) {
        if (!liveItem.title.isEmpty()) {
            live_title_label_->setText(liveItem.title);
            live_title_label_->setToolTip(liveItem.title);
        } else {
            const QString unknownRoomTitle = QStringLiteral("\u672A\u77E5\u76F4\u64AD\u95F4");
            live_title_label_->setText(unknownRoomTitle);
            live_title_label_->setToolTip(unknownRoomTitle);
        }
    }

    LOG_INFO(QString("LiveItem set - liveId: %1, title: %2, status: %3")
        .arg(liveItem.liveId)
        .arg(liveItem.title)
        .arg(liveItem.status == LiveStatus::LIVE ? "直播中" :
            liveItem.status == LiveStatus::PENDING ? "待开播" : "已结束")
        .toStdString());


    if (live_id_changed) {
        live_id_ = liveItem.liveId;


        if (listWidget_sceneItems_) {
            listWidget_sceneItems_->blockSignals(true);
            listWidget_sceneItems_->clear();
            listWidget_sceneItems_->blockSignals(false);
        }


        load_scenes_config();
    }


    if (!liveItem.pushUrl.isEmpty() && !liveItem.pushUrl[0].isEmpty()) {
        QString rtmpUrl = liveItem.pushUrl[0];

        // FIX: Only accept rtmp:// or rtmps:// URLs
        if (rtmpUrl.startsWith("rtmp://") || rtmpUrl.startsWith("rtmps://")) {
            rtmp_server_url_ = rtmpUrl;
        }
    }

    QString canvasOrientation = liveItem.orientationString();
    LOG_INFO(QString("Server canvas orientation: %1 (isPortraitMode=%2)")
        .arg(canvasOrientation).arg(liveItem.isPortraitMode()).toStdString());
    apply_server_canvas_config(canvasOrientation);


    if (ui->pushButton_toggleOrientation) {
        ui->pushButton_toggleOrientation->setText(is_portrait_mode_ ? "竖屏" : "横屏");
    }


    initWebEngineViews();
    LOG_INFO("========== setLiveItem END ==========");
}

void MainWindow::initialize_modules() {
    LOG_INFO("========== initialize_modules START ==========");


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



    gpu_compositor_ = std::make_shared<GpuCompositor>();
    gpu_color_converter_ = std::make_shared<GpuColorConverter>();
    const int canvas_w = canvas_config_.get_width();
    const int canvas_h = canvas_config_.get_height();
    if (gpu_compositor_->initialize(canvas_w, canvas_h)) {
        if (gpu_color_converter_->initialize(canvas_w, canvas_h, canvas_w, canvas_h)) {
            LOG_INFO("GPU pipeline (GpuCompositor + GpuColorConverter) initialized successfully");
        } else {
            LOG_WARNING("GpuColorConverter initialization failed, GPU path disabled");
            gpu_color_converter_.reset();
        }
    } else {
        LOG_WARNING("GpuCompositor initialization failed, GPU path disabled");
        gpu_compositor_.reset();
        gpu_color_converter_.reset();
    }


    build_scene_selector();


    load_scenes_config();


    LOG_INFO(QString("Initializing VideoEngine with resolution: %1x%2")
        .arg(canvas_config_.get_width()).arg(canvas_config_.get_height()).toStdString());
    video_engine_->initialize(canvas_config_.get_width(), canvas_config_.get_height(), 30);
    audio_engine_->initialize(48000, 2);
    app_settings_.audio.microphone_device_id = audio_engine_->refresh_microphone_to_system_default();
    app_settings_.audio.speaker_device_id = audio_engine_->refresh_speaker_to_system_default();
    LOG_INFO("[MainWindow] Synced audio devices to current system defaults: mic=" +
             app_settings_.audio.microphone_device_id +
             ", speaker=" + app_settings_.audio.speaker_device_id);
     LOG_INFO("Audio engine initialized, will start capture when live streaming begins");
    update_audio_status("待机", "gray");
    if (encoder_bridge_) {
        encoder_bridge_->set_audio_engine(audio_engine_);
        encoder_bridge_->set_silent_audio(true);  // 
    }


    loadAudioVolumeSettings();


    update_microphone_ui();
    update_speaker_ui();

    video_engine_->set_current_scene(scene_manager_->get_current_scene());

    VideoEncoderConfig video_config = build_video_config_from_settings(app_settings_);
    LOG_INFO(QString("Preparing deferred video encoder config: %1x%2").arg(video_config.width).arg(video_config.height).toStdString());
    encoder_->reinitialize_video_encoder(video_config);


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
        if (ui->label_liveDuration) ui->label_liveDuration->setText(QStringLiteral("\u76F4\u64AD\u65F6\u957F\uFF1A") + text);
    });


    system_info_timer_ = new QTimer(this);
    connect(system_info_timer_, &QTimer::timeout, [this]() {
        update_system_info();
    });


    system_log_timer_ = new QTimer(this);
    connect(system_log_timer_, &QTimer::timeout, [this]() {
        log_system_stats_periodically();
    });



    if (system_info_timer_) {
        QTimer::singleShot(500, this, [this]() {
            if (system_info_timer_) {
                system_info_timer_->start(3000);

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


        dlg.set_video_config(build_video_config_from_settings(app_settings_));


        {
            AudioEncoderConfig audio_for_dlg = app_settings_.audio.encoder_config;
            audio_for_dlg.mic_volume     = app_settings_.audio.mic_volume;
            audio_for_dlg.speaker_volume = app_settings_.audio.speaker_volume;
            dlg.set_audio_config(audio_for_dlg);
        }


        dlg.set_available_microphones(audio_engine_->get_available_microphones(),
                                       app_settings_.audio.microphone_device_id);


        dlg.set_available_speakers(audio_engine_->get_available_speakers(),
                                    app_settings_.audio.speaker_device_id);


        if (video_engine_) {
            dlg.set_available_cameras(video_engine_->get_available_camera_choices());
            dlg.set_video_engine(video_engine_);


            std::string current_camera_device_id = app_settings_.camera.device_id;
            std::string current_resolution       = video_engine_->get_camera_resolution();
            int         current_fps              = video_engine_->get_camera_fps();
            bool        current_mirror           = app_settings_.camera.mirror;

            dlg.set_camera_config(current_camera_device_id, current_resolution, current_fps, current_mirror);
        }


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


    if (ui->label_status) {
        ui->label_status->setText("预览中");
        ui->label_status->setObjectName("labelPreviewStatus");
    }


    if (ui->pushButton_toggleOrientation) {
        connect(ui->pushButton_toggleOrientation, &QPushButton::clicked, this, [this]() {
            toggle_canvas_orientation();

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

                        if (is_local_stream_mode_) {
                            QMessageBox::information(this, "提示", "本地推流模式不支持插播视频");
                            return;
                        }

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
        connect(ui->pushButton_startLive, &QPushButton::clicked, this, [this]() {
            if (!encoder_bridge_) {
                QMessageBox::warning(this, "错误", "推流系统未初始化");
                return;
            }

            if (encoder_bridge_->is_streaming()) {

                QMessageBox::StandardButton reply = QMessageBox::question(
                    this,
                    "结束直播",
                    "确认结束直播么？",
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No
                );

                if (reply == QMessageBox::Yes) {

                    encoder_bridge_->stop_streaming();

                    if (audio_engine_ && audio_engine_->is_capturing()) {
                        LOG_INFO("Stopping audio capture after live streaming ended");
                        audio_engine_->stop_capture();
                    }
                    ui->pushButton_startLive->setText("开始直播");

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

                    if (live_duration_timer_) {
                        live_duration_timer_->stop();
                        streaming_start_time_ms_ = 0;
                        if (ui->label_liveDuration) {
                            ui->label_liveDuration->setText(QStringLiteral("\u76F4\u64AD\u65F6\u957F\uFF1A00:00:00"));
                        }
                    }

                    if (system_info_timer_) {
                        system_info_timer_->start(3000);

                        update_system_info();
                    }
                    LOG_INFO("直播已结束");
                }

                return;
            }


            LOG_DEBUG("[DIAG] 准备开始推流");
            QMessageBox::StandardButton reply = QMessageBox::question(
                this,
                "开始直播",
                "确认开始直播么？",
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No
            );

            if (reply == QMessageBox::Yes) {

                QString url = rtmp_server_url_;
                if (url.isEmpty()) {
                    QMessageBox::warning(this, "错误", "推流地址未配置，请先选择直播间");
                    ui->pushButton_startLive->setEnabled(true);
                    ui->pushButton_startLive->setText("开始直播");
                    if (ui->label_status) ui->label_status->setText("");
                    return;
                }

                // Disable button immediately so UI stays responsive while
                // avio_open2 (RTMP connect, up to 8s timeout) runs in background.
                ui->pushButton_startLive->setEnabled(false);
                ui->pushButton_startLive->setText("正在连接...");
                if (ui->label_status) {
                    ui->label_status->setText("正在连接...");
                    ui->label_status->setStyleSheet("color: orange; font-weight: bold;");
                }

                QString url_copy = url;
                QThread* worker = QThread::create([this, url_copy]() {
                    bool ok = encoder_bridge_->start_streaming(url_copy.toStdString());
                    // Marshal result back to main thread
                    QMetaObject::invokeMethod(this, "on_start_streaming_finished",
                                             Qt::QueuedConnection,
                                             Q_ARG(bool, ok),
                                             Q_ARG(QString, url_copy));
                });
                worker->setParent(this);
                connect(worker, &QThread::finished, worker, &QThread::deleteLater);
                worker->start();
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
            playVolumeFeedbackSound();
        });

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

        connect(ui->slider_speaker, &QSlider::sliderReleased, this, [this]() {
            saveAudioVolumeSettings();
        });
    }

    // Initialize audio controls
    update_microphone_ui();
    update_speaker_ui();


    if (ui->pushButton_insertVideo) {
        if (is_local_stream_mode_) {

            ui->pushButton_insertVideo->setEnabled(false);
            ui->pushButton_insertVideo->setToolTip("本地推流模式不支持插播视频");
            LOG_INFO("Insert video button disabled for local stream mode");
        } else {
            connect(ui->pushButton_insertVideo, &QPushButton::clicked, this, &MainWindow::on_insert_video_button_clicked);
        }
    }
}

void MainWindow::setupBottomButtonsStyle() {
    if (ui->horizontalLayout_techStats &&
        ui->pushButton_shareScreen &&
        ui->pushButton_camera &&
        ui->pushButton_insertVideo) {
        QLayoutItem* spacerLayoutItem = ui->horizontalLayout_techStats->itemAt(1);
        if (spacerLayoutItem && spacerLayoutItem->spacerItem()) {
            spacerLayoutItem->spacerItem()->changeSize(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);
            ui->horizontalLayout_techStats->invalidate();
        }

        QWidget* actionButtonsContainer = findChild<QWidget*>("widget_bottomActionButtons");
        if (!actionButtonsContainer) {
            actionButtonsContainer = new QWidget(this);
            actionButtonsContainer->setObjectName("widget_bottomActionButtons");
            actionButtonsContainer->setMinimumWidth(280);
            actionButtonsContainer->setMaximumWidth(280);

            auto* actionButtonsLayout = new QHBoxLayout(actionButtonsContainer);
            actionButtonsLayout->setContentsMargins(0, 0, 0, 0);
            actionButtonsLayout->setSpacing(8);

            ui->horizontalLayout_techStats->removeWidget(ui->pushButton_shareScreen);
            ui->horizontalLayout_techStats->removeWidget(ui->pushButton_camera);
            ui->horizontalLayout_techStats->removeWidget(ui->pushButton_insertVideo);

            actionButtonsLayout->addWidget(ui->pushButton_shareScreen);
            actionButtonsLayout->addWidget(ui->pushButton_camera);
            actionButtonsLayout->addWidget(ui->pushButton_insertVideo);
            ui->horizontalLayout_techStats->addWidget(actionButtonsContainer);
            ui->horizontalLayout_techStats->invalidate();
        }
    }

    QString shareScreenStyle = R"(
        QPushButton {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, 
                                        stop:0 #2196F3, stop:1 #00BCD4);
            color: white;
            border-radius: 6px;
            padding: 4px 8px;
            font-size: 12px;
            font-weight: bold;
            min-width: 72px;
            max-width: 88px;
            min-height: 26px;
            max-height: 26px;
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


    QString cameraStyle = R"(
        QPushButton {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, 
                                        stop:0 #4CAF50, stop:1 #8BC34A);
            color: white;
            border-radius: 6px;
            padding: 4px 8px;
            font-size: 12px;
            font-weight: bold;
            min-width: 72px;
            max-width: 88px;
            min-height: 26px;
            max-height: 26px;
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


    QString insertVideoStyle = R"(
        QPushButton {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, 
                                        stop:0 #FF9800, stop:1 #FF5722);
            color: white;
            border-radius: 6px;
            padding: 4px 8px;
            font-size: 12px;
            font-weight: bold;
            min-width: 72px;
            max-width: 88px;
            min-height: 26px;
            max-height: 26px;
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


    if (ui->pushButton_shareScreen) {
        ui->pushButton_shareScreen->setStyleSheet(shareScreenStyle);
        ui->pushButton_shareScreen->setIconSize(QSize(14, 14));

        connect(ui->pushButton_shareScreen, &QPushButton::clicked, this, [this]() {
            LOG_INFO("Share screen button clicked from bottom toolbar");
            show_screen_share_selector();
        });
    }
    if (ui->pushButton_camera) {
        ui->pushButton_camera->setStyleSheet(cameraStyle);
        ui->pushButton_camera->setIconSize(QSize(14, 14));

        connect(ui->pushButton_camera, &QPushButton::clicked, this, [this]() {
            LOG_INFO("Camera button clicked from bottom toolbar");
            on_camera_button_clicked();
        });
    }
    if (ui->pushButton_insertVideo) {
        ui->pushButton_insertVideo->setStyleSheet(insertVideoStyle);
        ui->pushButton_insertVideo->setIconSize(QSize(14, 14));
    }
    if (ui->pushButton_settings) {
        ui->pushButton_settings->setStyleSheet(settingsStyle);
    }
    
    LOG_INFO("Bottom buttons styles applied with different colors");
}

void MainWindow::on_insert_video_button_clicked() {
    LOG_INFO("Insert video button clicked");


    if (is_local_stream_mode_) {
        QMessageBox::information(this, "提示", "本地推流模式不支持插播视频");
        return;
    }

    show_insert_video_widget();
}

void MainWindow::show_insert_video_widget() {
    if (!insert_video_widget_) {
        insert_video_widget_ = new InsertVideoWidget(this);

        connect(insert_video_widget_, &InsertVideoWidget::startInsertVideo,
                this, &MainWindow::on_start_insert_video);

        connect(insert_video_widget_, &InsertVideoWidget::requestPauseCurrentInsertVideo,
                this, [this]() {
                    pauseCurrentInsertVideo();
                    update_insert_video_widget_state();
                });
        connect(insert_video_widget_, &InsertVideoWidget::requestResumeInsertVideo,
                this, [this]() {
                    resumePausedInsertVideo();
                    update_insert_video_widget_state();
                });
        connect(insert_video_widget_, &InsertVideoWidget::requestStopCurrentInsertVideo,
                this, [this]() {
                    stopInsertVideoPlayback();
                    update_insert_video_widget_state();
                });
        connect(insert_video_widget_, &InsertVideoWidget::requestStopPausedInsertVideo,
                this, [this]() {
                    stopPausedInsertVideo();
                    update_insert_video_widget_state();
                });
    }

    // 每次打开都用当前直播间刷新，避免切换直播间后数据仍是旧房间的
    QString roomId = current_live_item_.liveId;
    if (!roomId.isEmpty()) {
        insert_video_widget_->setLiveInfo(live_url_, user_id_, token_, roomId);
    }

    // 同步当前插播状态到面板
    update_insert_video_widget_state();

    insert_video_widget_->show();
    insert_video_widget_->raise();
    insert_video_widget_->activateWindow();
}

void MainWindow::on_start_insert_video(const QString& fileId, const QString& fileName, bool loopEnabled) {
    LOG_INFO("Starting insert video: " + fileId.toStdString() + " - " + fileName.toStdString() +
             ", loopEnabled=" + std::to_string(loopEnabled));

    // 若当前有播放中的视频，暂停（冻结画面保留在画布，加入暂停列表）
    if (is_insert_video_playing_) {
        pauseCurrentInsertVideo();
    }

    startInsertVideoPlayback(fileId, loopEnabled);
    update_insert_video_widget_state();

    // 开始插播后自动关闭插播列表窗口，避免遮挡画布
    if (insert_video_widget_) {
        insert_video_widget_->hide();
    }
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

    // 二次校验：status 标记为已下载，但本地文件可能实际不存在
    // （如切换环境/缓存被清理），此时重置状态并重新下载
    {
        QString localPath = fileItem->getLocalCachePath();
        if (!QFile::exists(localPath)) {
            LOG_WARNING("Insert video cache file missing on disk, re-downloading: " + localPath.toStdString());
            fileItem->status = InsertFileStatus::TRANSCODE_SUCCEEDED;  // 重置为待下载
            InsertFileManager::instance()->startDownload(fileId);
            QMessageBox::information(this, "提示", "视频文件缓存已丢失，正在重新下载，请下载完成后再试");
            return;
        }
    }

    std::string source_id = "insert_video_" + fileId.toStdString();
    auto mediaSource = std::make_shared<MediaFileSource>(source_id, fileItem);


    mediaSource->set_loop_enabled(loopEnabled);


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


    if (!scene_manager_) {
        LOG_ERROR("Scene manager not initialized");
        return;
    }

    auto scene = scene_manager_->get_current_scene();
    if (!scene) {
        LOG_ERROR("No current scene");
        return;
    }


    // 记录 source 加入的场景（Bug1 fix）
    insert_video_scene_name_ = QString::fromStdString(scene->get_name());

    // [DIAG] 记录插播视频加入的场景
    LOG_INFO("[DIAG][START_INSERT] Adding insert video source_id=" + source_id
             + " to scene=" + scene->get_name());

    auto sceneItem = scene->add_source(mediaSource);
    if (sceneItem) {

        Transform transform = make_centered_fit_transform(
            canvas_config_.get_width(),
            canvas_config_.get_height(),
            1280,
            720,
            0.9,
            0.55);
        scene->set_transform(sceneItem, transform);
        

        sceneItem->set_order(0);


        if (compositor_) {
            const std::string source_id_str = mediaSource->get_id();
            if (!compositor_->has_layer(source_id_str)) {
                compositor_->add_layer(source_id_str);

                compositor_->set_layer_order(source_id_str, 0);
            }
            compositor_->update_layer_transform(source_id_str,
                QRectF(transform.x, transform.y, transform.width, transform.height), 1.0f);
        }




        if (encoder_bridge_) {
            encoder_bridge_->attach_insert_video_source(mediaSource.get());
        }


        if (mediaSource->start()) {
            current_insert_video_source_ = mediaSource;
            current_insert_video_file_id_ = fileId;
            is_insert_video_playing_ = true;


            insert_video_timer_ = new QTimer(this);
            connect(insert_video_timer_, &QTimer::timeout, this, &MainWindow::on_insert_video_frame_ready);
            insert_video_timer_->start(33);  // ~30fps
            LOG_INFO("[INSERT_VIDEO] Frame sync timer started at 30fps");

            if (audio_engine_) {
                audio_engine_->clearMediaFrames();
                audio_engine_->set_media_mute(false);
                audio_engine_->set_media_volume(0.7f);
            }
            update_audio_mix_mode();

            LOG_INFO("Insert video playback started: " + fileItem->fileName.toStdString());


            sync_scene_to_compositor();


            auto scene = scene_manager_->get_current_scene();
            if (scene) {
                auto items = scene->get_all_scene_items();
                for (auto& item : items) {
                    if (item && item->get_source()) {
                        auto src = item->get_source();

                        if (src->get_type() == Source::Type::VIDEO_CAPTURE) {
                            item->set_order(9999);
                        }
                    }
                }
                scene->normalize_orders();

                sync_scene_to_compositor();
            }

            build_scene_list();



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

    // [DIAG] 停止插播诊断：记录当前场景 vs source所在场景
    {
        std::string current_scene_name = "null";
        if (scene_manager_ && scene_manager_->get_current_scene()) {
            current_scene_name = scene_manager_->get_current_scene()->get_name();
        }
        std::string source_id = current_insert_video_source_ ? current_insert_video_source_->get_id() : "null";
        // 检查source实际在哪个场景（通过scene_names遍历查找）
        std::string source_scene = "not_found_in_any_scene";
        if (scene_manager_ && current_insert_video_source_) {
            std::string sid = current_insert_video_source_->get_id();
            for (const auto& sname : scene_manager_->get_scene_names()) {
                auto items = scene_manager_->get_scene_items(sname);
                for (const auto& item : items) {
                    if (item && item->get_source() && item->get_source()->get_id() == sid) {
                        source_scene = sname;
                        break;
                    }
                }
                if (source_scene != "not_found_in_any_scene") break;
            }
        }
        bool mismatch = (source_scene != "not_found_in_any_scene") && (current_scene_name != source_scene);
        LOG_INFO("[DIAG][STOP_INSERT] current_scene=" + current_scene_name
                 + " | source_id=" + source_id
                 + " | source_actually_in_scene=" + source_scene
                 + " | MISMATCH=" + std::to_string(mismatch));
    }

    LOG_INFO("Stopping insert video playback");


    if (insert_video_timer_) {
        insert_video_timer_->stop();
        insert_video_timer_->deleteLater();
        insert_video_timer_ = nullptr;
    }

    // Bug2 fix: 先 stop 解码线程（不再产帧），再 detach 清队列（安全无竞态）
    if (current_insert_video_source_) {
        current_insert_video_source_->stop();
    }

    if (encoder_bridge_ && current_insert_video_source_) {
        encoder_bridge_->detach_insert_video_source(current_insert_video_source_.get());
    }

    // Bug1 fix: 从 source 实际所在的场景删除，而非当前激活场景
    if (scene_manager_ && current_insert_video_source_ && !insert_video_scene_name_.isEmpty()) {
        scene_manager_->remove_source_from_scene(
            insert_video_scene_name_.toStdString(),
            current_insert_video_source_->get_id());
    }

    if (current_insert_video_source_) {
        current_insert_video_source_->shutdown();
        current_insert_video_source_.reset();
    }

    if (audio_engine_) {
        audio_engine_->set_media_mute(true);
        audio_engine_->clearMediaFrames();
    }

    current_insert_video_file_id_.clear();
    insert_video_scene_name_.clear();
    insert_video_last_frame_.reset();
    is_insert_video_playing_ = false;

    // 同时清理所有暂停中的视频（完全停止时不保留任何暂停槽）
    for (auto& entry : paused_insert_videos_) {
        if (!entry.source) continue;
        if (scene_manager_ && !entry.scene_name.isEmpty()) {
            scene_manager_->remove_source_from_scene(
                entry.scene_name.toStdString(), entry.source->get_id());
        }
        entry.source->shutdown();
    }
    paused_insert_videos_.clear();

    update_audio_mix_mode();

    sync_scene_to_compositor();
    build_scene_list();

    LOG_INFO("Insert video playback stopped");
}

void MainWindow::on_stop_insert_video() {
    stopInsertVideoPlayback();
}

void MainWindow::pauseCurrentInsertVideo() {
    if (!is_insert_video_playing_ || !current_insert_video_source_) return;

    LOG_INFO("Pausing insert video (frozen in canvas): " + current_insert_video_file_id_.toStdString());

    // 1. 停止定时器
    if (insert_video_timer_) {
        insert_video_timer_->stop();
        insert_video_timer_->deleteLater();
        insert_video_timer_ = nullptr;
    }

    // 2. Bug2 fix 顺序：先 stop 解码线程，再 detach 清队列
    current_insert_video_source_->stop();
    if (encoder_bridge_) {
        encoder_bridge_->detach_insert_video_source(current_insert_video_source_.get());
    }

    // 3. 追加到暂停列表（保留在场景中，画面冻结在最后一帧，记录断点位置）
    PausedInsertEntry entry;
    entry.source              = current_insert_video_source_;
    entry.file_id             = current_insert_video_file_id_;
    entry.scene_name          = insert_video_scene_name_;
    entry.frozen_frame        = insert_video_last_frame_;
    entry.paused_position_ms  = current_insert_video_source_->get_current_position_ms();
    paused_insert_videos_.push_back(std::move(entry));

    current_insert_video_source_.reset();
    current_insert_video_file_id_.clear();
    insert_video_scene_name_.clear();
    is_insert_video_playing_ = false;

    if (audio_engine_) {
        audio_engine_->set_media_mute(true);
        audio_engine_->clearMediaFrames();
    }
    update_audio_mix_mode();
    build_scene_list();

    LOG_INFO("Insert video paused");
}

void MainWindow::resumePausedInsertVideo() {
    // 恢复最近暂停的那个（列表末尾）
    if (paused_insert_videos_.empty()) return;
    resumeSpecificInsertVideo(paused_insert_videos_.back().source->get_id());
}

void MainWindow::resumeSpecificInsertVideo(const std::string& source_id) {
    // 在暂停列表中找到目标 entry
    auto it = std::find_if(paused_insert_videos_.begin(), paused_insert_videos_.end(),
        [&source_id](const PausedInsertEntry& e) {
            return e.source && e.source->get_id() == source_id;
        });
    if (it == paused_insert_videos_.end()) {
        LOG_WARNING("resumeSpecificInsertVideo: source not found in paused list: " + source_id);
        return;
    }

    // 若当前有正在播放的视频，先暂停（加入列表，不完全停止）
    if (is_insert_video_playing_) {
        pauseCurrentInsertVideo();
        // 重新查找（pauseCurrentInsertVideo 修改了 vector）
        it = std::find_if(paused_insert_videos_.begin(), paused_insert_videos_.end(),
            [&source_id](const PausedInsertEntry& e) {
                return e.source && e.source->get_id() == source_id;
            });
        if (it == paused_insert_videos_.end()) return;
    }

    LOG_INFO("Resuming specific insert video: " + it->file_id.toStdString() +
             " from position " + std::to_string(it->paused_position_ms) + "ms");

    auto source                  = it->source;
    QString file_id              = it->file_id;
    QString scene_name           = it->scene_name;
    int64_t resume_position_ms   = it->paused_position_ms;

    // 从暂停列表中移除（先保存再 erase，避免悬空引用）
    paused_insert_videos_.erase(it);

    // attach encoder bridge
    if (encoder_bridge_) {
        encoder_bridge_->attach_insert_video_source(source.get());
    }

    // 从断点位置快速重启（保留硬件解码器上下文，无冷启动延迟）
    if (source->seek_and_restart(resume_position_ms)) {
        current_insert_video_source_  = source;
        current_insert_video_file_id_ = file_id;
        insert_video_scene_name_      = scene_name;
        is_insert_video_playing_      = true;

        insert_video_timer_ = new QTimer(this);
        connect(insert_video_timer_, &QTimer::timeout, this, &MainWindow::on_insert_video_frame_ready);
        insert_video_timer_->start(33);

        if (audio_engine_) {
            audio_engine_->clearMediaFrames();
            audio_engine_->set_media_mute(false);
            audio_engine_->set_media_volume(0.7f);
        }
        update_audio_mix_mode();
        build_scene_list();

        LOG_INFO("Insert video resumed from position " + std::to_string(resume_position_ms) + "ms");
    } else {
        LOG_ERROR("Failed to restart media source: " + source_id);
        // 恢复失败：把这个 source 从场景中完全移除
        if (scene_manager_ && !scene_name.isEmpty()) {
            scene_manager_->remove_source_from_scene(scene_name.toStdString(), source_id);
        }
        source->shutdown();
        sync_scene_to_compositor();
    }
}

void MainWindow::stopPausedInsertVideo() {
    // 停止最近暂停的那个（列表末尾）
    if (paused_insert_videos_.empty()) return;

    PausedInsertEntry entry = std::move(paused_insert_videos_.back());
    paused_insert_videos_.pop_back();

    LOG_INFO("Stopping paused insert video: " + entry.file_id.toStdString());

    // 从其所在场景移除
    if (scene_manager_ && !entry.scene_name.isEmpty()) {
        scene_manager_->remove_source_from_scene(
            entry.scene_name.toStdString(), entry.source->get_id());
    }

    // 移除 compositor layer
    if (compositor_) {
        const std::string sid = entry.source->get_id();
        if (compositor_->has_layer(sid)) {
            compositor_->remove_layer(sid);
        }
    }

    entry.source->shutdown();

    sync_scene_to_compositor();
    build_scene_list();

    LOG_INFO("Paused insert video removed from canvas");
}

void MainWindow::update_insert_video_widget_state() {
    if (!insert_video_widget_) return;

    QString playingFileId, playingFileName, pausedFileId, pausedFileName;
    if (is_insert_video_playing_ && !current_insert_video_file_id_.isEmpty()) {
        playingFileId = current_insert_video_file_id_;
        auto fileItem = InsertFileManager::instance()->getFile(playingFileId);
        if (fileItem) playingFileName = fileItem->fileName;
    }
    // 面板只展示最近暂停的那个（列表末尾）
    if (!paused_insert_videos_.empty()) {
        pausedFileId = paused_insert_videos_.back().file_id;
        auto fileItem = InsertFileManager::instance()->getFile(pausedFileId);
        if (fileItem) pausedFileName = fileItem->fileName;
    }
    insert_video_widget_->setCurrentInsertState(
        playingFileId, playingFileName, pausedFileId, pausedFileName);
}

void MainWindow::on_insert_video_frame_ready() {

    if (!encoder_bridge_ || !compositor_ || !current_insert_video_source_) {
        return;
    }

    const std::string source_id = current_insert_video_source_->get_id();
    bool in_scene = is_source_in_current_scene(source_id);

    // [DIAG] 帧就绪诊断：每秒记录一次 source 是否在当前场景
    {
        static int64_t last_diag_time = 0;
        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now_ms - last_diag_time >= 2000) {
            std::string current_scene_name = "null";
            if (scene_manager_ && scene_manager_->get_current_scene()) {
                current_scene_name = scene_manager_->get_current_scene()->get_name();
            }
            LOG_INFO("[DIAG][FRAME_READY] source_id=" + source_id
                     + " | current_scene=" + current_scene_name
                     + " | is_source_in_current_scene=" + std::to_string(in_scene));
            last_diag_time = now_ms;
        }
    }

    if (!in_scene) {
        return;
    }

    SyncedVideoFrame synced_frame;

    if (encoder_bridge_->pop_insert_video_frame(synced_frame) && synced_frame.frame) {
        compositor_->update_layer_video_frame(source_id, synced_frame.frame);
        insert_video_last_frame_ = synced_frame.frame;  // 保存最后一帧，供暂停时画面冻结用


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


    for (const auto& camera : camera_choices) {
        LOG_INFO("摄像头设备: " + camera.display_name + " (DShow名称: " + camera.dshow_name + ")");
    }


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


    if (has_running_camera) {
        LOG_INFO("已有摄像头在运行，禁用预览功能");
        dialog.set_preview_disabled(true);
    }


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


        const std::string source_id = "camera_" + std::to_string(std::hash<std::string>{}(camera_device_id));
        if (capture_manager_ && capture_manager_->has_source(source_id)) {
            LOG_INFO("该摄像头已在使用中，无需重新添加");
            QMessageBox::information(this, "提示", "该摄像头已在使用中");
            return;
        }


        auto preview_source = dialog.take_preview_source();
        if (preview_source) {
            LOG_INFO("复用对话框已打开的摄像头源");

            on_select_camera_with_source(selected_camera, capture_cfg, preview_source);
        } else {

            LOG_INFO("创建新的摄像头源");
            on_select_camera(selected_camera, capture_cfg);
        }
    }

}

void MainWindow::on_select_camera(const QString& camera_name, const CaptureConfig& config) {
    LOG_INFO("Selected camera: '" + camera_name.toStdString() +
             "' mode=" + capture_mode_to_string(config.capture_mode));

    if (!capture_manager_) {
        LOG_ERROR("采集管理器未初始化");
        QMessageBox::warning(this, "错误", "采集管理器未初始化");
        return;
    }


    std::string camera_device_id = config.target_id;
    if (config.capture_mode == CaptureMode::OPENCV) {

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

    connect(src.get(), &ICaptureSource::frameReady, this, [this, source_id](const CaptureFrame& frame) {

        if (compositor_ && !frame.image.isNull() && is_source_in_current_scene(source_id)) {
            if (!compositor_->has_layer(source_id)) {
                compositor_->add_layer(source_id);


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


        if (scene_manager_ && scene_manager_->get_current_scene()) {
            auto scene = scene_manager_->get_current_scene();
            auto items = scene->get_all_scene_items();
            for (auto& item : items) {
                if (item && item->get_source_id() == source_id) {

                    auto screenSrc = std::dynamic_pointer_cast<ScreenSource>(item->get_source());
                    if (screenSrc) {
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


                    auto cameraSrc = std::dynamic_pointer_cast<CameraSource>(item->get_source());
                    if (cameraSrc) {
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

                added_item->set_device_id(camera_device_id);
                added_item->set_display_name(camera_name.toStdString());


                std::unordered_map<std::string, std::string> params;


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


                params["ffmpeg_device_id"] = camera_device_id;


                params["opencv_index"] = "0";
                added_item->set_source_params(params);


                // Get mirror setting from video engine
                bool mirror = false;
                if (video_engine_) {
                    mirror = video_engine_->get_camera_mirror();
                }

                int src_w = config.width > 0 ? config.width : 1280;
                int src_h = config.height > 0 ? config.height : 720;
                Transform tr = make_centered_fit_transform(
                    canvas_config_.get_width(),
                    canvas_config_.get_height(),
                    src_w,
                    src_h,
                    0.9,
                    0.5,
                    mirror);
                scene->set_transform(added_item, tr);


                added_item->set_order(9999);
                scene->normalize_orders();
            }
            LOG_INFO("更新场景项并同步到Compositor");
            update_scene_items();
            LOG_INFO("Added camera source to scene: " + camera_name.toStdString() + ", id=" + source_id);
        }


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


    std::string camera_device_id = config.target_id;
    if (config.capture_mode == CaptureMode::OPENCV) {
        camera_device_id = std::to_string(config.opencv_index);
    }

    const std::string source_id = "camera_" + std::to_string(std::hash<std::string>{}(camera_device_id));
    LOG_INFO("生成的源ID: " + source_id);


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


    LOG_INFO("连接frameReady信号到Compositor的槽函数");
    connect(existing_source.get(), &ICaptureSource::frameReady, this, [this, source_id](const CaptureFrame& frame) {
        LOG_DEBUG("[DIAG] 收到frameReady信号，源ID: " + source_id);


        if (compositor_ && !frame.image.isNull() && is_source_in_current_scene(source_id)) {
            if (!compositor_->has_layer(source_id)) {
                compositor_->add_layer(source_id);


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


    LOG_INFO("将采集源添加到采集管理器: " + source_id);
    capture_manager_->add_source(source_id, existing_source);
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
        if (added_item) {

            added_item->set_device_id(camera_device_id);
            added_item->set_display_name(camera_name.toStdString());


            std::unordered_map<std::string, std::string> params;
            params["resolution"] = config.resolution_string();
            params["fps"] = std::to_string(config.fps);
            params["pixel_format"] = pixel_format_to_string(config.pixel_format);
            params["capture_mode"] = capture_mode_to_string(config.capture_mode);
            params["mirror"] = config.mirror ? "true" : "false";
            params["ffmpeg_device_id"] = config.target_id;
            params["opencv_index"] = std::to_string(config.opencv_index);
            added_item->set_source_params(params);

            int src_w = config.width > 0 ? config.width : 1280;
            int src_h = config.height > 0 ? config.height : 720;
            Transform tr = make_centered_fit_transform(
                canvas_config_.get_width(),
                canvas_config_.get_height(),
                src_w,
                src_h,
                0.9,
                0.5,
                config.mirror);
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

                connect(src.get(), &ICaptureSource::frameReady, this, [this, source_id](const CaptureFrame& frame) {




                    if (source_id.find("capture_") == 0) {

                        if (compositor_ && !frame.image.isNull() && is_source_in_current_scene(source_id)) {
                            if (!compositor_->has_layer(source_id)) {
                                compositor_->add_layer(source_id);


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


                        if (scene_manager_ && scene_manager_->get_current_scene()) {
                            auto scene = scene_manager_->get_current_scene();
                            auto items = scene->get_all_scene_items();
                            for (auto& item : items) {
                                if (item && item->get_source_id() == source_id) {

                                    auto screenSrc = std::dynamic_pointer_cast<ScreenSource>(item->get_source());
                                    if (screenSrc) {
                                        screenSrc->push_frame(frame.image);
                                        break;
                                    }


                                    auto cameraSrc = std::dynamic_pointer_cast<CameraSource>(item->get_source());
                                    if (cameraSrc) {
                                        cameraSrc->push_frame(frame.image);
                                        break;
                                    }
                                }
                            }
                        }
                    }

                }, Qt::QueuedConnection);

                // WGC 初始化失败（如安全软件触发 E_ACCESSDENIED）时弹出用户提示
                connect(src.get(), &ICaptureSource::captureError, this,
                        [this](const QString& /*source_id*/, const QString& error_message) {
                    QMessageBox::warning(this, "屏幕共享失败", error_message);
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

                            added_item->set_device_id(selected_target->id);


                            std::unordered_map<std::string, std::string> params;
                            params["fps"] = std::to_string(fps);
                            params["capture_cursor"] = capture_cursor ? "true" : "false";
                            params["capture_border"] = capture_border ? "true" : "false";
                            added_item->set_source_params(params);


                            int canvas_w = canvas_config_.get_width();
                            int canvas_h = canvas_config_.get_height();



                            int src_w = 1280;
                            int src_h = 720;


                            double scale_w = static_cast<double>(canvas_w) / src_w;
                            double scale_h = static_cast<double>(canvas_h) / src_h;
                            double scale = (std::min)(scale_w, scale_h);


                            int target_w = static_cast<int>(src_w * scale);
                            int target_h = static_cast<int>(src_h * scale);


                            int x = (canvas_w - target_w) / 2;
                            int y = (canvas_h - target_h) / 2;

                            Transform tr(x, y, target_w, target_h, 0.0f, 1.0f);
                            scene->set_transform(added_item, tr);
                        }


                        auto items = scene->get_all_scene_items();
                        for (auto& item : items) {
                            if (QString::fromStdString(item->get_source_id()).startsWith("camera_")) {
                                item->set_order(9999);
                            }
                        }
                        scene->normalize_orders();

                        update_scene_items();


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


    if (capture_manager_ && capture_manager_->has_source(source_id)) {
        LOG_INFO("Screen sharing already active for: " + target_id.toStdString());
    } else {
        LOG_INFO("Screen share target selected, WGC capture started for: " + target_id.toStdString());
    }
}

void MainWindow::on_camera_frame_ready() {
}

void MainWindow::update_status(const QString& message) {

}

void MainWindow::setup_canvas_widget() {
    LOG_INFO("========== setup_canvas_widget START ==========");
    canvas_widget_ = new CanvasWidget(this);
    canvas_widget_->set_scene_manager(scene_manager_);
    canvas_widget_->set_video_engine(video_engine_);

    LOG_INFO("Calling set_canvas_config from setup_canvas_widget");
    set_canvas_config(canvas_config_);

    canvas_widget_->set_compositor(compositor_);

    encoder_bridge_->set_compositor(compositor_);

    if (gpu_compositor_ && gpu_color_converter_) {
        encoder_bridge_->set_gpu_compositor(gpu_compositor_);
        encoder_bridge_->set_gpu_color_converter(gpu_color_converter_);
        LOG_INFO("GPU pipeline wired to encoder bridge");
    }

    if (canvas_widget_->get_renderer() && scene_manager_->get_current_scene()) {
        // 传入非拥有裸指针：CanvasRenderer 由 canvas_widget_->renderer_（unique_ptr）独占管理
        // 切勿用 shared_ptr(raw_ptr) 包装，否则与 unique_ptr 形成双重所有权 → double-free
        encoder_bridge_->set_canvas_renderer(
            canvas_widget_->get_renderer(),
            scene_manager_->get_current_scene()
        );
    }
    encoder_bridge_->set_encoder(encoder_);
    encoder_bridge_->set_stream_pusher(stream_pusher_);
    encoder_bridge_->set_audio_engine(audio_engine_);

    encoder_bridge_->set_resolution(canvas_config_.get_width(), canvas_config_.get_height());
    // Start the encoder bridge so it begins capturing/compositing frames for streaming.
    if (encoder_ && encoder_bridge_) {
        encoder_bridge_->set_fps(static_cast<int>(encoder_->get_video_config().fps));
        encoder_bridge_->start(static_cast<int>(encoder_->get_video_config().fps));
        LOG_INFO("Encoder bridge started from MainWindow with fps: " + std::to_string(encoder_->get_video_config().fps));
    }


    connect(encoder_bridge_.get(), &CompositorEncoderBridge::streaming_started,
            this, &MainWindow::on_streaming_started);
    connect(encoder_bridge_.get(), &CompositorEncoderBridge::streaming_stopped,
            this, &MainWindow::on_streaming_stopped);
    // QueuedConnection: start_streaming() holds state_mutex_ when it emits
    // streaming_error on failure.  A DirectConnection would call on_streaming_error
    // immediately in the same thread, which re-enters state_mutex_ (non-recursive)
    // → undefined behaviour / crash in kernelbase.  QueuedConnection defers the
    // slot to the next event-loop iteration after start_streaming() has returned
    // and released the lock.
    connect(encoder_bridge_.get(), &CompositorEncoderBridge::streaming_error,
            this, &MainWindow::on_streaming_error,
            Qt::QueuedConnection);

    connect(encoder_bridge_.get(), &CompositorEncoderBridge::streaming_reconnecting,
            this, [this](int attempt, int max_attempts) {
                if (ui->pushButton_startLive) {
                    ui->pushButton_startLive->setText(
                        QString("正在重连 (%1/%2)").arg(attempt).arg(max_attempts));
                    ui->pushButton_startLive->setEnabled(false);
                }
                if (ui->label_status) {
                    ui->label_status->setText("断线重连中...");
                    ui->label_status->setStyleSheet("color: orange; font-weight: bold;");
                }
            }, Qt::QueuedConnection);

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


    if (!canvasContainer_) {
        canvasContainer_ = new QWidget(ui->centralWidget);
    }
    canvasContainer_->setObjectName("liveCanvasContainer");
    canvasContainer_->setAttribute(Qt::WA_StyledBackground, true);
    ui->verticalLayout_liveArea->insertWidget(0, canvasContainer_);


    canvas_widget_->setParent(canvasContainer_);

    canvas_widget_->hide();


    ui->liveArea->updateGeometry();
    ui->liveArea->layout()->activate();
    canvasContainer_->updateGeometry();


    QTimer::singleShot(0, this, [this]() {

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

    repositionPlaceholderOverlays();

    connect(canvas_widget_, &CanvasWidget::scene_item_selected, this, [this](std::shared_ptr<SceneItem> item) {
        LOG_INFO("Scene item selected: " + (item ? item->get_source_id() : "null"));
    });

    connect(canvas_widget_, &CanvasWidget::scene_item_moved, this,
        [this](std::shared_ptr<SceneItem>, const Transform&, const Transform&) {
            sync_scene_to_compositor();
        });

    // 单击画布上的插播视频：点哪个激活哪个，其余自动暂停
    // 双击已恢复为全屏/最大化（见 canvas.cpp mouseDoubleClickEvent）
    connect(canvas_widget_, &CanvasWidget::insert_video_single_clicked, this,
        [this](std::shared_ptr<SceneItem> item) {
            if (!item) return;
            const std::string clicked_id = item->get_source_id();

            if (current_insert_video_source_ &&
                current_insert_video_source_->get_id() == clicked_id) {
                // 点击的是正在播放的那个 → 暂停
                // seek skip 期间拒绝暂停：此时 is_running()=true 但画面仍冻结（解码跳帧中）
                // 若此时允许暂停，用户刚点击播放后因为"看起来没反应"再次点击会误触发暂停
                if (current_insert_video_source_->is_seeking()) {
                    LOG_INFO("[INSERT_VIDEO] Click ignored: still seeking (seek skip in progress)");
                    return;
                }
                pauseCurrentInsertVideo();
            } else {
                // 点击的是某个暂停中的 → 激活它（当前播放的自动暂停）
                resumeSpecificInsertVideo(clicked_id);
            }
            update_insert_video_widget_state();
        });

    LOG_INFO("Canvas widget setup completed");
    // After canvas inserted, update placeholder visibility
    updateStagePlaceholderVisibility();


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

    system_monitor().update();


    const auto& sys_stats = system_monitor().get_cached_stats();


    QString memory_text;
    {

        memory_text = QString("内存: %1GB/%2GB (%3%)")
            .arg(sys_stats.memory_used_bytes / 1024.0 / 1024.0 / 1024.0, 0, 'f', 1)
            .arg(sys_stats.memory_total_bytes / 1024.0 / 1024.0 / 1024.0, 0, 'f', 1)
            .arg(sys_stats.memory_usage_percent, 0, 'f', 0);
    }


    QString status_text;
    QString style;


    bool is_pushing = stream_pusher_ && stream_pusher_->is_pushing();

    if (is_pushing) {

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


    if (QString::fromStdString(source_id).startsWith("camera_")) {

        if (!encoder_ || !audio_engine_ || !video_engine_) {
            QMessageBox::warning(this, "错误", "模块未初始化");
            return;
        }

        SettingsPanel dlg(this, SettingsTab::Camera);


        dlg.set_video_config(build_video_config_from_settings(app_settings_));


        {
            AudioEncoderConfig audio_for_dlg = app_settings_.audio.encoder_config;
            audio_for_dlg.mic_volume     = app_settings_.audio.mic_volume;
            audio_for_dlg.speaker_volume = app_settings_.audio.speaker_volume;
            dlg.set_audio_config(audio_for_dlg);
        }


        dlg.set_available_microphones(audio_engine_->get_available_microphones(),
                                       app_settings_.audio.microphone_device_id);


        dlg.set_available_speakers(audio_engine_->get_available_speakers(),
                                    app_settings_.audio.speaker_device_id);


        dlg.set_available_cameras(video_engine_->get_available_camera_choices());


        std::string current_resolution = video_engine_->get_camera_resolution();
        int         current_fps        = video_engine_->get_camera_fps();
        bool        current_mirror     = app_settings_.camera.mirror;
        std::string current_device_id  = app_settings_.camera.device_id;
        if (current_device_id.empty()) {
            current_device_id = item->get_device_id();
        }


        dlg.set_camera_config(current_device_id, current_resolution, current_fps, current_mirror);

        if (dlg.exec() == QDialog::Accepted) {

            applySettingsPanelChanges(dlg);


            std::string new_resolution = dlg.get_camera_resolution();
            int new_fps = dlg.get_camera_fps();
            bool new_mirror = dlg.is_camera_mirror();

            LOG_INFO("摄像头设置更新 - 分辨率: " + new_resolution +
                     ", 帧率: " + std::to_string(new_fps) +
                     ", 镜像: " + (new_mirror ? "开启" : "关闭"));


            video_engine_->set_camera_resolution(new_resolution);
            video_engine_->set_camera_fps(new_fps);
            video_engine_->set_camera_mirror(new_mirror);


            if (current_mirror != new_mirror) {
                auto transform = item->get_transform();
                transform.mirror = new_mirror;
                item->set_transform(transform);
            }


            if (new_resolution != current_resolution || new_fps != current_fps) {
                QMessageBox::information(this, "提示",
                    "分辨率或帧率已更改，需要重新添加摄像头才能生效。\n"
                    "如需应用这些更改，请删除当前摄像头后重新添加。");
            }

            LOG_INFO("Camera settings updated for source: " + source_id);
        }
    }

    else if (QString::fromStdString(source_id).startsWith("capture_")) {
        LOG_INFO("Opening share settings for source: " + source_id);


        ShareSettingsDialog dlg(this);


        if (capture_manager_) {
            auto src = capture_manager_->get_source(source_id);
            if (src) {
                LOG_INFO("Found source in capture_manager, current config - cursor: " +
                         std::string(src->get_config().capture_cursor ? "true" : "false") +
                         ", border: " + std::string(src->get_config().capture_border ? "true" : "false"));

                const auto& cfg = src->get_config();
                dlg.setInitialValues(cfg.capture_cursor, cfg.capture_border);
            } else {
                LOG_WARNING("Source not found in capture_manager: " + source_id);
            }
        } else {
            LOG_WARNING("capture_manager_ is null");
        }

        if (dlg.exec() == QDialog::Accepted) {
            bool capture_cursor = dlg.isCaptureCursor();
            bool capture_border = dlg.isCaptureBorder();

            LOG_INFO("Share settings dialog accepted - cursor: " + std::string(capture_cursor ? "true" : "false") +
                     ", border: " + std::string(capture_border ? "true" : "false"));


            if (capture_manager_) {
                capture_manager_->update_share_settings(source_id, capture_cursor, capture_border);
            }

            QMessageBox::information(this, "提示", "共享设置已更新。");
        } else {
            LOG_INFO("Share settings dialog cancelled");
        }
    } else {

        QMessageBox::information(this, "提示", "设置功能开发中...");
    }
}

void MainWindow::SettingsApplier::on_settings_changed(const AppSettings& s, SettingsSection changed) {
    MainWindow* w = owner_;

    if (has_section(changed, SettingsSection::Video)) {
        VideoEncoderConfig video_config = MainWindow::build_video_config_from_settings(s);
        if (w->encoder_) {
            w->encoder_->reinitialize_video_encoder(video_config);
        }
        if (w->encoder_bridge_) {
            w->encoder_bridge_->set_resolution(video_config.width, video_config.height);
            w->encoder_bridge_->set_fps(video_config.fps);
        }
    }

    if (has_section(changed, SettingsSection::Canvas)) {
        w->canvas_config_ = s.canvas;
        w->is_portrait_mode_ = (s.canvas.get_width() < s.canvas.get_height());
        if (w->canvas_widget_) {
            w->canvas_widget_->set_canvas_config(s.canvas);
        }

        if (!has_section(changed, SettingsSection::Video)) {
            VideoEncoderConfig video_config = MainWindow::build_video_config_from_settings(s);
            if (w->encoder_) {
                w->encoder_->reinitialize_video_encoder(video_config);
            }
            if (w->encoder_bridge_) {
                w->encoder_bridge_->set_resolution(video_config.width, video_config.height);
            }
        }
    }

    if (has_section(changed, SettingsSection::Audio)) {
        const auto& a = s.audio;
        if (w->audio_engine_) {
            w->audio_engine_->initialize(a.encoder_config.sample_rate, a.encoder_config.channels);
            w->audio_engine_->set_microphone_volume(a.mic_volume);
            w->audio_engine_->set_speaker_volume(a.speaker_volume);
            if (!a.microphone_device_id.empty()) {
                w->audio_engine_->select_microphone(a.microphone_device_id);
            }
            if (!a.speaker_device_id.empty()) {
                w->audio_engine_->select_speaker(a.speaker_device_id);
            }
        }
        if (w->encoder_) {
            w->encoder_->reinitialize_audio_encoder(a.encoder_config);
        }
    }

    if (has_section(changed, SettingsSection::Camera)) {
        if (w->video_engine_) {
            w->video_engine_->set_camera_mirror(s.camera.mirror);
        }

        if (w->scene_manager_ && w->scene_manager_->get_current_scene()) {
            auto scene = w->scene_manager_->get_current_scene();
            for (auto& item : scene->get_all_scene_items()) {
                if (item && item->get_source() &&
                    QString::fromStdString(item->get_source()->get_id()).startsWith("camera_")) {
                    Transform tr = item->get_transform();
                    tr.mirror = s.camera.mirror;
                    scene->set_transform(item, tr);
                    break;
                }
            }
        }
    }

    if (has_section(changed, SettingsSection::UIState)) {
        w->exit_preference_ = s.ui_state.exit_preference;
    }
}

void MainWindow::applySettingsPanelChanges(SettingsPanel& dlg) {
    SettingsSection changed = SettingsSection::None;


    app_settings_.video = dlg.get_video_config();
    changed |= SettingsSection::Video;


    const auto new_v = app_settings_.video;
    if (canvas_config_.get_width() != new_v.width || canvas_config_.get_height() != new_v.height) {
        LOG_INFO(QString("分辨率改变: %1x%2 -> %3x%4")
            .arg(canvas_config_.get_width()).arg(canvas_config_.get_height())
            .arg(new_v.width).arg(new_v.height).toStdString());
        app_settings_.canvas = (new_v.width >= new_v.height)
            ? CanvasConfig(CanvasConfig::DisplayMode::LANDSCAPE_16_9)
            : CanvasConfig(CanvasConfig::DisplayMode::PORTRAIT_9_16);
        changed |= SettingsSection::Canvas;
    }


    app_settings_.audio.encoder_config      = dlg.get_audio_config();
    app_settings_.audio.mic_volume          = dlg.get_microphone_volume();
    app_settings_.audio.speaker_volume      = dlg.get_speaker_volume();
    app_settings_.audio.microphone_device_id = dlg.get_selected_microphone_id();
    app_settings_.audio.speaker_device_id   = dlg.get_selected_speaker_id();
    changed |= SettingsSection::Audio;


    app_settings_.camera.mirror     = dlg.is_camera_mirror();
    app_settings_.camera.device_id  = dlg.get_selected_camera_id();
    app_settings_.camera.resolution = dlg.get_camera_resolution();
    app_settings_.camera.fps        = dlg.get_camera_fps();
    changed |= SettingsSection::Camera;


    app_settings_.save(changed);
    app_settings_.notify(changed);


    if (stream_pusher_ && stream_pusher_->is_pushing()) {
        QMessageBox::information(this, "提示", "参数已修改，将重启推流使其生效");
        if (encoder_bridge_) encoder_bridge_->stop();
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

    // --- 插播视频特殊处理 ---
    // 若删除的是正在播放的插播视频：停止解码/音频，清除播放状态
    // （scene/compositor 的移除由后续通用逻辑完成，不重复调用 stopInsertVideoPlayback）
    if (is_insert_video_playing_ && current_insert_video_source_ &&
        current_insert_video_source_->get_id() == sid) {
        LOG_INFO("[delete_scene_item] Deleting active insert video source: " + sid);

        if (insert_video_timer_) {
            insert_video_timer_->stop();
            insert_video_timer_->deleteLater();
            insert_video_timer_ = nullptr;
        }
        current_insert_video_source_->stop();
        if (encoder_bridge_) {
            encoder_bridge_->detach_insert_video_source(current_insert_video_source_.get());
        }
        current_insert_video_source_->shutdown();
        current_insert_video_source_.reset();
        if (audio_engine_) {
            audio_engine_->set_media_mute(true);
            audio_engine_->clearMediaFrames();
        }
        current_insert_video_file_id_.clear();
        insert_video_scene_name_.clear();
        insert_video_last_frame_.reset();
        is_insert_video_playing_ = false;
        update_audio_mix_mode();
    }

    // 若删除的是暂停中的插播视频：从暂停列表移除并释放
    auto paused_it = std::find_if(paused_insert_videos_.begin(), paused_insert_videos_.end(),
        [&sid](const PausedInsertEntry& e) {
            return e.source && e.source->get_id() == sid;
        });
    if (paused_it != paused_insert_videos_.end()) {
        LOG_INFO("[delete_scene_item] Deleting paused insert video source: " + sid);
        paused_it->source->shutdown();
        paused_insert_videos_.erase(paused_it);
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

    update_insert_video_widget_state();
    update_scene_items();
    if (canvas_widget_) canvas_widget_->refresh();
}

QString MainWindow::extract_source_name(std::shared_ptr<Source> source, std::shared_ptr<SceneItem> item) {
    if (!source) return "Unknown";


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

std::shared_ptr<SceneItem> MainWindow::find_current_scene_item(const std::string& source_id) const {
    if (!scene_manager_) {
        return nullptr;
    }

    auto scene = scene_manager_->get_current_scene();
    if (!scene) {
        return nullptr;
    }

    const auto items = scene->get_all_scene_items();
    for (const auto& item : items) {
        if (item && item->get_source_id() == source_id) {
            return item;
        }
    }

    return nullptr;
}

bool MainWindow::is_source_in_current_scene(const std::string& source_id) const {
    return find_current_scene_item(source_id) != nullptr;
}

void MainWindow::sync_scene_to_compositor() {
    if (!compositor_ || !scene_manager_ || !scene_manager_->get_current_scene()) {
        return;
    }

    auto scene = scene_manager_->get_current_scene();




    auto items = scene->get_all_scene_items();


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


        if (w < canvas_width) {
            final_x = (std::max)(0, (std::min)(tr.x, canvas_width - w));
        }
        if (h < canvas_height) {
            final_y = (std::max)(0, (std::min)(tr.y, canvas_height - h));
        }


        compositor_->update_layer_transform(sid, QRectF(final_x, final_y, final_w, final_h), tr.opacity);
        compositor_->set_layer_visible(sid, it->is_visible());
        compositor_->set_layer_order(sid, it->get_order());
    }

    // 收集属于当前激活场景的暂停视频 source ID
    // 注意：只保留当前场景的暂停视频 layer，其他场景的暂停视频 layer 需要移除
    // 否则切场景后，旧场景的冻结画面会叠加在新场景画面上
    std::string current_scene_name = scene ? scene->get_name() : "";
    std::unordered_set<std::string> paused_sids_in_current_scene;
    for (const auto& entry : paused_insert_videos_) {
        if (entry.source && entry.scene_name.toStdString() == current_scene_name) {
            paused_sids_in_current_scene.insert(entry.source->get_id());
        }
    }

    for (const auto& lid : compositor_->get_layer_ids()) {
        if (active.find(lid) == active.end()) {
            // 只保留属于当前场景的暂停视频 layer，其他场景的直接移除
            if (paused_sids_in_current_scene.count(lid)) {
                continue;
            }
            compositor_->remove_layer(lid);
        }
    }

    // 恢复属于当前场景的暂停视频冻结帧（防止 layer 重建后变黑）
    for (const auto& entry : paused_insert_videos_) {
        if (!entry.source || !entry.frozen_frame) continue;
        if (entry.scene_name.toStdString() != current_scene_name) continue;
        const std::string sid = entry.source->get_id();
        if (compositor_->has_layer(sid)) {
            compositor_->update_layer_video_frame(sid, entry.frozen_frame);
        }
    }
}

void MainWindow::update_audio_status(const QString& text, const QString& color) {


    LOG_INFO("Audio status: " + text.toStdString() + " (color: " + color.toStdString() + ")");
}

void MainWindow::toggle_microphone() {
    microphone_enabled_ = !microphone_enabled_;
    if (audio_engine_) {
        audio_engine_->set_microphone_mute(!microphone_enabled_);
    }
    update_audio_mix_mode();
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

        audio_engine_->get_audio_capturer()->set_speaker_capture_enabled(speaker_enabled_);
        audio_engine_->set_speaker_mute(!speaker_enabled_);
        if (!speaker_enabled_) {
            audio_engine_->clearSpeakerFrames();
        }
    }
    update_audio_mix_mode();
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

void MainWindow::update_audio_mix_mode() {
    if (!audio_engine_) {
        return;
    }

    const bool include_mic = microphone_enabled_;
    const bool include_speaker =
        speaker_enabled_ &&
        audio_engine_->get_audio_capturer() &&
        audio_engine_->get_audio_capturer()->is_speaker_capture_enabled();
    const bool include_media = is_insert_video_playing_;

    AudioMixMode mode = AudioMixMode::MIC_ONLY;
    if (include_mic && include_speaker && include_media) {
        mode = AudioMixMode::MIC_SPEAKER_MEDIA;
    } else if (include_mic && include_speaker) {
        mode = AudioMixMode::MIC_SPEAKER;
    } else if (include_mic && include_media) {
        mode = AudioMixMode::MIC_MEDIA;
    } else if (include_speaker && include_media) {
        mode = AudioMixMode::SPEAKER_MEDIA;
    } else if (include_speaker) {
        mode = AudioMixMode::SPEAKER_ONLY;
    } else if (include_media) {
        mode = AudioMixMode::MEDIA_ONLY;
    }

    audio_engine_->setMixMode(mode);
    LOG_INFO("[MainWindow] Audio mix mode updated: mic=" +
             std::string(include_mic ? "on" : "off") +
             ", speaker=" + std::string(include_speaker ? "on" : "off") +
             ", media=" + std::string(include_media ? "on" : "off") +
             ", mode=" + std::to_string(static_cast<int>(mode)));
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

                update_speaker_ui();
            }
        });
    }

    menu.exec(pos);
}



void MainWindow::on_streaming_started() {
    LOG_INFO("推流状态：已开始");
    streaming_start_time_ms_ = QDateTime::currentMSecsSinceEpoch();
    if (ui->label_liveDuration) ui->label_liveDuration->setText(QStringLiteral("\u76F4\u64AD\u65F6\u957F\uFF1A00:00:00"));

    if (live_duration_timer_) live_duration_timer_->start(200);


    if (system_info_timer_) {
        system_info_timer_->start(1000);

        update_system_info();
    }


    if (system_log_timer_) {
        system_log_timer_->start(60000);
        system_log_counter_ = 0;

        log_system_stats_periodically();
    }
}

void MainWindow::on_streaming_stopped() {
    LOG_INFO("推流状态：已停止");
    if (live_duration_timer_) live_duration_timer_->stop();

    if (system_info_timer_) {
        system_info_timer_->start(3000);

        update_system_info();
    }
    if (system_log_timer_) system_log_timer_->stop();
    streaming_start_time_ms_ = 0;
    if (ui->label_liveDuration) ui->label_liveDuration->setText(QStringLiteral("\u76F4\u64AD\u65F6\u957F\uFF1A00:00:00"));


    update_system_info();
}

void MainWindow::on_streaming_error(const QString& error) {
    LOG_ERROR("推流错误: " + error.toStdString());


    if (encoder_bridge_ && encoder_bridge_->is_streaming()) {
        encoder_bridge_->stop_streaming();
    }
    if (stream_pusher_ && stream_pusher_->is_pushing()) {
        stream_pusher_->stop();
    }


    if (audio_engine_ && audio_engine_->is_capturing()) {
        audio_engine_->stop_capture();
        update_audio_status("已停止", "gray");
    }


    if (encoder_bridge_) {
        encoder_bridge_->stop();
    }


    if (ui->pushButton_startLive) {
        ui->pushButton_startLive->setText("开始直播");
        ui->pushButton_startLive->setEnabled(true);
    }
    if (ui->label_status) {
        ui->label_status->setText("推流失败");
        ui->label_status->setStyleSheet("color: red; font-weight: bold;");
    }

    // 显示中文错误提示，隐藏内部错误码
    QString userMsg;
    if (error.contains("reconnect") || error.contains("重连") || error.contains("断线")) {
        userMsg = "推流断线，重连失败，请检查网络后重新开播";
    } else if (error.contains("connect") || error.contains("NOT_CONNECTED") || error.contains("server")) {
        userMsg = "连接推流服务器失败，请检查推流地址和网络";
    } else {
        userMsg = "推流出现错误，请重新开播";
    }
    QMessageBox::warning(this, "推流错误", userMsg);
}

void MainWindow::on_start_streaming_finished(bool ok, const QString& url) {
    if (!ok) {
        // Restore button immediately so the UI doesn't stay stuck on "正在连接..."
        // even if the streaming_error QueuedConnection signal is slightly delayed.
        if (ui->pushButton_startLive) {
            ui->pushButton_startLive->setText("开始直播");
            ui->pushButton_startLive->setEnabled(true);
        }
        // streaming_error signal (QueuedConnection) handles the popup and full cleanup.
        return;
    }

    if (audio_engine_ && !audio_engine_->is_capturing()) {
        LOG_INFO("Starting audio capture for live streaming");
        update_audio_status("初始化中...", "orange");
        bool audio_started = false;
        for (int i = 0; i < 3 && !audio_started; ++i) {
            if (i > 0) QThread::msleep(500);
            if (audio_engine_->start_capture()) audio_started = true;
        }
        if (!audio_started) {
            LOG_WARNING("Failed to start audio capture for live streaming");
            update_audio_status("故障", "red");
        } else {
            if (encoder_bridge_) encoder_bridge_->set_silent_audio(false);
            update_audio_status("正常", "green");
        }
    }

    ui->pushButton_startLive->setEnabled(true);
    ui->pushButton_startLive->setText("停止直播");

    if (ui->pushButton_toggleOrientation) {
        ui->pushButton_toggleOrientation->setEnabled(false);
        ui->pushButton_toggleOrientation->setStyleSheet(
            "QPushButton { background: #666666; color: #999999; border: none; border-radius: 4px; font-size: 12px; font-weight: bold; }"
        );
    }
    if (ui->label_status) {
        ui->label_status->setText("正在推流");
        ui->label_status->setStyleSheet("");
    }

    streaming_start_time_ms_ = QDateTime::currentMSecsSinceEpoch();
    if (live_duration_timer_) live_duration_timer_->start(200);
    if (system_info_timer_) system_info_timer_->start(2000);
    LOG_INFO("推流已启动: " + url.toStdString());
}

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


    if (canvas_widget_) {
        LOG_INFO("Updating canvas_widget_ config");
        canvas_widget_->set_canvas_config(config);
    }



    if (encoder_ && encoder_bridge_ && canvasContainer_) {
        LOG_INFO(QString("Reinitializing encoder with resolution: %1x%2")
                 .arg(config.get_width()).arg(config.get_height()).toStdString());

        app_settings_.canvas = config;
        VideoEncoderConfig video_config = build_video_config_from_settings(app_settings_);
        encoder_->reinitialize_video_encoder(video_config);
        encoder_bridge_->set_resolution(config.get_width(), config.get_height());
    } else {
        LOG_INFO("Skipping encoder reinitialization (initialization phase)");
    }

    LOG_INFO("Canvas config updated to: " + config.get_name());
    LOG_INFO("========== set_canvas_config END ==========");
}

const CanvasConfig& MainWindow::get_canvas_config() const {
    return canvas_config_;
}


void MainWindow::set_landscape_mode() {
    LOG_INFO("========== set_landscape_mode START ==========");
    if (!is_portrait_mode_) {
        LOG_INFO("Already in landscape mode, returning");
        return;
    }


    if (encoder_bridge_ && encoder_bridge_->is_streaming()) {
        LOG_INFO("Streaming in progress, cannot switch");
        QMessageBox::warning(this, "画布切换",
            "正在直播推流中，无法切换画布方向。\n"
            "请先停止直播后再切换。");
        return;
    }

    LOG_INFO("Switching to landscape mode (1280x720)");


    canvas_config_ = CanvasConfig::get_default();
    is_portrait_mode_ = false;


    apply_canvas_config_change();


    if (ui->pushButton_toggleOrientation) {
        ui->pushButton_toggleOrientation->setText("横屏");
    }

    LOG_INFO("Switched to landscape mode: 1280x720");
    LOG_INFO("========== set_landscape_mode END ==========");
}


void MainWindow::set_portrait_mode() {
    LOG_INFO("========== set_portrait_mode START ==========");
    if (is_portrait_mode_) {
        LOG_INFO("Already in portrait mode, returning");
        return;
    }


    if (encoder_bridge_ && encoder_bridge_->is_streaming()) {
        LOG_INFO("Streaming in progress, cannot switch");
        QMessageBox::warning(this, "画布切换",
            "正在直播推流中，无法切换画布方向。\n"
            "请先停止直播后再切换。");
        return;
    }

    LOG_INFO("Switching to portrait mode (720x1280)");


    canvas_config_ = CanvasConfig::get_portrait();
    is_portrait_mode_ = true;


    apply_canvas_config_change();


    if (ui->pushButton_toggleOrientation) {
        ui->pushButton_toggleOrientation->setText("竖屏");
    }

    LOG_INFO("Switched to portrait mode: 720x1280");
    LOG_INFO("========== set_portrait_mode END ==========");
}


void MainWindow::toggle_canvas_orientation() {

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


void MainWindow::apply_server_canvas_config(const QString& orientation) {
    LOG_INFO("========== apply_server_canvas_config START ==========");
    server_canvas_orientation_ = orientation.toLower();

    LOG_INFO("Applying server canvas config: " + server_canvas_orientation_.toStdString());
    LOG_INFO(QString("Current mode before apply: %1").arg(is_portrait_mode_ ? "portrait" : "landscape").toStdString());

    if (server_canvas_orientation_ == "portrait") {
        set_portrait_mode();
    } else {

        set_landscape_mode();
    }
    LOG_INFO("========== apply_server_canvas_config END ==========");
}


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


    if (canvas_widget_) {
        LOG_INFO("Updating CanvasWidget");
        canvas_widget_->set_canvas_config(canvas_config_);
    }


    if (compositor_) {
        LOG_INFO("Updating Compositor");
        compositor_->set_canvas_size(width, height);
    }


    if (encoder_bridge_) {
        LOG_INFO("Updating EncoderBridge");
        encoder_bridge_->set_resolution(width, height);
    }


    if (video_engine_) {
        LOG_INFO("Reinitializing VideoEngine");
        video_engine_->initialize(width, height, 30);
    }


    if (encoder_ && encoder_bridge_) {
        // 无论是否推流，始终更新编码器配置
        // 1. 若编码器未激活：reinitialize_video_encoder 仅存储配置，等待下次启动时应用（延迟初始化）
        // 2. 若编码器已激活：使用内部 video_mutex_ 保护，安全地重新初始化
        // 调用者（set_landscape_mode/set_portrait_mode）已在推流时提前拦截，此处不需要重复检查
        LOG_INFO("Reinitializing video encoder");
        app_settings_.canvas = canvas_config_;
        VideoEncoderConfig video_config = build_video_config_from_settings(app_settings_);
        encoder_->reinitialize_video_encoder(video_config);
        LOG_INFO("Video encoder reinitialized for " + std::string(is_portrait_mode_ ? "portrait" : "landscape") +
                 " mode: " + std::to_string(width) + "x" + std::to_string(height));
    }


    LOG_INFO("Adjusting scene items");
    adjust_scene_items_for_canvas_change();



    if (current_insert_video_source_ && compositor_) {
        const std::string source_id = current_insert_video_source_->get_id();
        if (source_id.find("insert_video_") == 0 && compositor_->has_layer(source_id)) {
            QRectF fullscreen_rect(0, 0, width, height);
            compositor_->update_layer_transform(source_id, fullscreen_rect, 1.0f);
            LOG_INFO("Updated insert video layer transform for portrait mode: " +
                     std::to_string(width) + "x" + std::to_string(height));
        }
    }


    LOG_INFO("Updating canvas orientation UI");
    update_canvas_orientation_ui();
    LOG_INFO("========== apply_canvas_config_change END ==========");
}


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


        if (transform.x + transform.width > canvas_w) {
            transform.x = (std::max)(0, canvas_w - transform.width);
        }
        if (transform.y + transform.height > canvas_h) {
            transform.y = (std::max)(0, canvas_h - transform.height);
        }


        if (transform.width > canvas_w) {
            transform.width = canvas_w / 2;
        }
        if (transform.height > canvas_h) {
            transform.height = canvas_h / 2;
        }


        transform.x = (std::min)(transform.x, canvas_w - transform.width);
        transform.y = (std::min)(transform.y, canvas_h - transform.height);

        scene->set_transform(item, transform);
    }


    sync_scene_to_compositor();


    if (canvas_widget_) {
        canvas_widget_->refresh();
    }

    LOG_INFO("Scene items adjusted for new canvas size: " +
             std::to_string(canvas_w) + "x" + std::to_string(canvas_h));
}


void MainWindow::update_canvas_orientation_ui() {

    LOG_INFO("Canvas orientation UI updated");


    update_stage_container_aspect_ratio();
}


void MainWindow::update_stage_container_aspect_ratio() {
    if (!ui || !ui->liveArea) {
        return;
    }


    double canvas_aspect = canvas_config_.get_aspect_ratio();


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


    int target_width, target_height;


    int height_by_width = static_cast<int>(available_width / canvas_aspect);

    int width_by_height = static_cast<int>(available_height * canvas_aspect);

    if (height_by_width <= available_height) {

        target_width = available_width;
        target_height = height_by_width;
    } else {

        target_width = width_by_height;
        target_height = available_height;
    }


    target_width = qMax(target_width, 320);
    target_height = qMax(target_height, 180);


    int x = (available_width - target_width) / 2;
    int y = (available_height - target_height) / 2;


    if (canvasContainer_ && canvas_widget_) {

        canvas_widget_->setGeometry(x, y, target_width, target_height);


        repositionPlaceholderOverlays();

        LOG_INFO("Canvas widget resized for " + std::string(is_portrait_mode_ ? "portrait" : "landscape") +
                 " mode: " + std::to_string(target_width) + "x" + std::to_string(target_height) +
                 " (aspect: " + std::to_string(canvas_aspect) + ")");
        return;
    }


    if (stageContainer_ && stagePlaceholderWidget_) {

        stageContainer_->setGeometry(x, y, target_width, target_height);


        if (canvas_widget_) {
            canvas_widget_->setGeometry(0, 0, target_width, target_height);
        }


        if (stageAddButton_) {
            stageAddButton_->setGeometry(stageContainer_->rect());
        }
    }

    LOG_INFO("Stage container resized for " + std::string(is_portrait_mode_ ? "portrait" : "landscape") +
             " mode: " + std::to_string(target_width) + "x" + std::to_string(target_height) +
             " (aspect: " + std::to_string(canvas_aspect) + ")");
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);

    if (!isMaximized() && !isMinimized() && event && event->size().isValid()) {
        last_normal_window_size_ = event->size();
    }

    scheduleDpiRelayout(false);
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
    

    if (is_exiting_) {
        event->accept();
        return;
    }
    

    if (encoder_bridge_ && encoder_bridge_->is_streaming()) {
        int ret = QMessageBox::question(this, "推流进行中",
            "当前正在推流直播中，确定要退出吗？\n退出后将中断直播推流。",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret == QMessageBox::No) {
            event->ignore();
            return;
        }
    }
    

    ExitDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        ExitDialog::Action action = dialog.getSelectedAction();
        

        if (dialog.shouldRememberChoice()) {
            if (action == ExitDialog::Action::Minimize) {
                saveExitPreference(1);
            } else {
                saveExitPreference(2);
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
            

            if (live_duration_timer_) live_duration_timer_->stop();
            if (system_info_timer_) system_info_timer_->stop();
            if (encoding_timer_) encoding_timer_->stop();
            

            if (encoder_bridge_ && encoder_bridge_->is_streaming()) {
                encoder_bridge_->stop_streaming();
            }


            saveAudioVolumeSettings();


            save_scenes_config();


            cleanupSystemTray();
            event->accept();
            QMainWindow::close();
        }
    } else {
        event->ignore();
    }
}

void MainWindow::setupSystemTray() {

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        LOG_WARNING("System tray is not available on this system");
        return;
    }
    

    system_tray_menu_ = new QMenu(this);
    

    tray_action_show_ = new QAction("显示窗口", this);
    connect(tray_action_show_, &QAction::triggered, this, &MainWindow::onTrayShowAction);
    system_tray_menu_->addAction(tray_action_show_);

    system_tray_menu_->addSeparator();

    tray_action_logout_ = new QAction("退出登录", this);
    connect(tray_action_logout_, &QAction::triggered, this, &MainWindow::onTrayLogoutAction);
    system_tray_menu_->addAction(tray_action_logout_);

    system_tray_menu_->addSeparator();

    tray_action_exit_ = new QAction("退出程序", this);
    connect(tray_action_exit_, &QAction::triggered, this, &MainWindow::onTrayExitAction);
    system_tray_menu_->addAction(tray_action_exit_);
    

    system_tray_icon_ = new QSystemTrayIcon(this);
    

    QIcon trayIcon(":/images/logo_new.ico");
    if (!trayIcon.isNull()) {
        system_tray_icon_->setIcon(trayIcon);
    } else {

        system_tray_icon_->setIcon(QIcon::fromTheme("application-default-icon"));
    }
    
    system_tray_icon_->setToolTip("灵犀");
    system_tray_icon_->setContextMenu(system_tray_menu_);
    


    tray_icon_initializing_ = true;
    connect(system_tray_icon_, &QSystemTrayIcon::activated,
            this, &MainWindow::onTrayIconActivated);
    

    system_tray_icon_->show();
    

    QTimer::singleShot(500, this, [this]() {
        tray_icon_initializing_ = false;
        LOG_INFO("Tray icon initialized, ready to respond to user clicks");
    });

    LOG_INFO("System tray icon initialized");
}

void MainWindow::setupNetworkConnections() {

    NetworkManager* networkManager = NetworkManager::instance();


    connect(networkManager, &NetworkManager::insertVideoTranscoded,
            this, [this](const QString& fileId, int fileState) {
        LOG_INFO(QString("Insert video transcoded: fileId=%1, state=%2").arg(fileId).arg(fileState).toStdString());

        InsertFileManager::instance()->refreshInsertFiles(current_live_item_.liveId);
    });


    connect(networkManager, &NetworkManager::startInsertVideo,
            this, [this](const QString& fileId) {
        LOG_INFO(QString("Received start insert video command: fileId=%1").arg(fileId).toStdString());

        if (!is_insert_video_playing_) {

            auto fileItem = InsertFileManager::instance()->getFile(fileId);
            if (fileItem && fileItem->isDownloaded()) {

                bool loopEnabled = fileItem->loopEnabled;
                startInsertVideoPlayback(fileId, loopEnabled);
            } else {
                LOG_WARNING("Insert video file not ready: " + fileId.toStdString());
            }
        }
    });


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
    tray_action_logout_ = nullptr;
    tray_action_exit_ = nullptr;
}

void MainWindow::onTrayIconActivated(QSystemTrayIcon::ActivationReason reason) {

    if (tray_icon_initializing_) {
        LOG_DEBUG("Ignoring tray icon activation during initialization");
        return;
    }
    
    switch (reason) {
    case QSystemTrayIcon::Trigger:
    case QSystemTrayIcon::DoubleClick:

        onTrayShowAction();
        break;
    case QSystemTrayIcon::MiddleClick:

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
    

    setWindowFlags(windowFlags() & ~Qt::Tool);
    show();
}

void MainWindow::onTrayLogoutAction() {
    if (encoder_bridge_ && encoder_bridge_->is_streaming()) {
        encoder_bridge_->stop_streaming();
    }
    cleanupSystemTray();
    emit request_logout();
    QMainWindow::close();
}

void MainWindow::onTrayExitAction() {
    is_exiting_ = true;
    

    if (live_duration_timer_) live_duration_timer_->stop();
    if (system_info_timer_) system_info_timer_->stop();
    if (encoding_timer_) encoding_timer_->stop();
    

    if (encoder_bridge_ && encoder_bridge_->is_streaming()) {
        encoder_bridge_->stop_streaming();
    }
    

    cleanupSystemTray();
    QMainWindow::close();
}

void MainWindow::handleExit() {
    LOG_INFO("User requested to exit live room");


    if (encoder_bridge_ && encoder_bridge_->is_streaming()) {
        int ret = QMessageBox::question(this, "推流进行中",
            "当前正在推流直播中，确定要退出吗？\n退出后将中断直播推流。",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret == QMessageBox::No) {
            return;
        }

        encoder_bridge_->stop_streaming();
    }


    save_scenes_config();


    stop_all_capture_sources();


    if (audio_engine_) {
        audio_engine_->stop_capture();
    }


    emit request_return_to_live_list();
}

void MainWindow::loadExitPreference() {
    exit_preference_ = app_settings_.ui_state.exit_preference;
    LOG_INFO("Loaded exit preference: " + std::to_string(exit_preference_));
}

void MainWindow::saveExitPreference(int preference) {
    exit_preference_ = preference;
    app_settings_.ui_state.exit_preference = preference;
    app_settings_.save(SettingsSection::UIState);
    LOG_INFO("Saved exit preference: " + std::to_string(preference));
}

void MainWindow::saveAudioVolumeSettings() {
    if (audio_engine_) {
        app_settings_.audio.mic_volume     = audio_engine_->get_microphone_volume();
        app_settings_.audio.speaker_volume = audio_engine_->get_speaker_volume();
        app_settings_.audio.mic_enabled    = microphone_enabled_;
        app_settings_.audio.speaker_enabled = speaker_enabled_;
        app_settings_.save(SettingsSection::Audio);
        LOG_INFO("Saved audio volume settings");
    }
}

void MainWindow::loadAudioVolumeSettings() {
    QSettings settings("LiveAssistant", "Settings");

    float micVolume;
    float speakerVolume;

    
    float sysMicVol     = audio_engine_ ? audio_engine_->get_system_microphone_volume() : -1.0f;
    float sysSpeakerVol = audio_engine_ ? audio_engine_->get_system_speaker_volume()    : -1.0f;

    if (sysMicVol >= 0.0f) {
        micVolume = sysMicVol;
        LOG_INFO("Loaded microphone volume from system: " + std::to_string(static_cast<int>(micVolume * 100)) + "%");
    } else if (settings.contains("audio/micVolume")) {
        micVolume = app_settings_.audio.mic_volume;
        LOG_INFO("Loaded microphone volume from user config: " + std::to_string(static_cast<int>(micVolume * 100)) + "%");
    } else {
        micVolume = 0.5f;
        LOG_INFO("Microphone volume: using default 50%");
    }

    if (sysSpeakerVol >= 0.0f) {
        speakerVolume = sysSpeakerVol;
        LOG_INFO("Loaded speaker volume from system: " + std::to_string(static_cast<int>(speakerVolume * 100)) + "%");
    } else if (settings.contains("audio/speakerVolume")) {
        speakerVolume = app_settings_.audio.speaker_volume;
        LOG_INFO("Loaded speaker volume from user config: " + std::to_string(static_cast<int>(speakerVolume * 100)) + "%");
    } else {
        speakerVolume = 0.5f;
        LOG_INFO("Speaker volume: using default 50%");
    }

    microphone_enabled_ = app_settings_.audio.mic_enabled;
    speaker_enabled_    = app_settings_.audio.speaker_enabled;


    app_settings_.audio.mic_volume      = micVolume;
    app_settings_.audio.speaker_volume  = speakerVolume;
    app_settings_.audio.mic_enabled     = microphone_enabled_;
    app_settings_.audio.speaker_enabled = speaker_enabled_;

    if (audio_engine_) {
        audio_engine_->set_microphone_volume(micVolume);
        audio_engine_->set_speaker_volume(speakerVolume);
        audio_engine_->set_microphone_mute(!microphone_enabled_);
        audio_engine_->set_speaker_mute(!speaker_enabled_);
        if (audio_engine_->get_audio_capturer()) {
            audio_engine_->get_audio_capturer()->set_speaker_capture_enabled(speaker_enabled_);
        }
    }

    update_audio_mix_mode();

    LOG_INFO("Loaded audio volume settings: mic=" + std::to_string(static_cast<int>(micVolume * 100)) +
             "%, speaker=" + std::to_string(static_cast<int>(speakerVolume * 100)) + "%");
}

void MainWindow::playVolumeFeedbackSound() {

}

// static
VideoEncoderConfig MainWindow::build_video_config_from_settings(const AppSettings& s) {
    VideoEncoderConfig cfg = s.video;

    cfg.width  = s.canvas.get_width();
    cfg.height = s.canvas.get_height();

    if (cfg.height > cfg.width && cfg.bitrate > 2000000) {
        cfg.bitrate = 2000000;
    }
    cfg.gop = cfg.fps * 2;
    cfg.b_frames_enabled = false;
    return cfg;
}

} // namespace live_assistant
