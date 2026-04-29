#include "app/insert_video_widget.h"
#include "app/insert_file_manager.h"
#include "http/live_item.h"
#include "common/log.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QTableWidgetItem>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPixmap>
#include <QFile>
#include <QFrame>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QPointer>
#include <QTimer>

InsertVideoWidget::InsertVideoWidget(QWidget *parent)
    : QDialog(parent), is_previewing_(false) {
    setupUI();
    setWindowTitle(QString::fromUtf8("插播视频"));

    // 连接 InsertFileManager 信号
    auto manager = InsertFileManager::instance();
    connect(manager, &InsertFileManager::filesUpdated, this, &InsertVideoWidget::onFilesUpdated);
    connect(manager, &InsertFileManager::downloadProgress, this, &InsertVideoWidget::onDownloadProgress);
    connect(manager, &InsertFileManager::downloadFinished, this, &InsertVideoWidget::onDownloadFinished);
    connect(manager, &InsertFileManager::errorOccurred, this, &InsertVideoWidget::onErrorOccurred);
}

InsertVideoWidget::~InsertVideoWidget() {
    // 析构时直接重置VLC播放器
    // 停止和资源释放在VlcPlayer析构函数中处理
    vlc_player_.reset();
}

void InsertVideoWidget::setupUI() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);
    mainLayout->setContentsMargins(15, 15, 15, 15);

    
    setMinimumSize(1100, 650);
    resize(1100, 650);
    setModal(true);

    // 标题栏
    auto* titleLayout = new QHBoxLayout();
    auto* titleLabel = new QLabel(QString::fromUtf8("添加插播视频"), this);
    titleLabel->setStyleSheet("font-size: 16px; font-weight: bold; color: #ffffff;");
    titleLayout->addWidget(titleLabel);
    titleLayout->addStretch();
    mainLayout->addLayout(titleLayout);

    // 搜索栏
    auto* searchLayout = new QHBoxLayout();
    searchEdit_ = new QLineEdit(this);
    searchEdit_->setPlaceholderText(QString::fromUtf8("请输入视频名称搜索"));
    searchEdit_->setMinimumHeight(36);
    searchEdit_->setStyleSheet(
        "QLineEdit { background-color: #2a2a2a; color: #ffffff; border: 1px solid #444444; "
        "border-radius: 6px; padding: 8px 12px; font-size: 14px; }"
        "QLineEdit:focus { border-color: #4a6ef0; }"
    );
    connect(searchEdit_, &QLineEdit::textChanged, this, &InsertVideoWidget::onSearchTextChanged);
    searchLayout->addWidget(searchEdit_);

    refreshButton_ = new QPushButton(QString::fromUtf8("🔄 刷新"), this);
    refreshButton_->setMinimumSize(90, 36);
    refreshButton_->setStyleSheet(
        "QPushButton { background-color: #3a5a6a; color: #ffffff; border-radius: 6px; padding: 8px 16px; }"
        "QPushButton:hover { background-color: #4a6a7a; }"
    );
    connect(refreshButton_, &QPushButton::clicked, this, &InsertVideoWidget::onRefreshClicked);
    searchLayout->addWidget(refreshButton_);
    mainLayout->addLayout(searchLayout);

    // 内容区域 - 左右布局
    auto* contentLayout = new QHBoxLayout();
    contentLayout->setSpacing(10);

    // 左侧：视频列表
    auto* leftLayout = new QVBoxLayout();
    leftLayout->setSpacing(10);

    // 视频列表表格
    tableWidget_ = new QTableWidget(this);
    tableWidget_->setColumnCount(6);
    tableWidget_->setHorizontalHeaderLabels({
        QString::fromUtf8(""),
        QString::fromUtf8("文件名称"),
        QString::fromUtf8("大小"),
        QString::fromUtf8("时长"),
        QString::fromUtf8("循环"),
        QString::fromUtf8("状态")
    });
    tableWidget_->setColumnWidth(0, 50);    // 复选框
    tableWidget_->setColumnWidth(1, 280);    // 文件名
    tableWidget_->setColumnWidth(2, 90);     // 大小
    tableWidget_->setColumnWidth(3, 80);     // 时长
    tableWidget_->setColumnWidth(4, 80);     // 循环
    tableWidget_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableWidget_->setSelectionMode(QAbstractItemView::SingleSelection);
    tableWidget_->setStyleSheet(
        "QTableWidget { background-color: #252525; border: 1px solid #3a3a3a; "
        "border-radius: 8px; color: #ffffff; gridline-color: #3a3a3a; }"
        "QTableWidget::item { padding: 6px 8px; border-bottom: 1px solid #3a3a3a; }"
        "QTableWidget::item:selected { background-color: #3a5a8a; }"
        "QTableWidget::item:hover { background-color: #2a3a4a; }"
        "QHeaderView::section { background-color: #2a2a2a; color: #cccccc; "
        "padding: 10px 8px; border: none; border-bottom: 1px solid #3a3a3a; "
        "font-weight: bold; }"
    );
    tableWidget_->horizontalHeader()->setStretchLastSection(true);
    tableWidget_->verticalHeader()->setVisible(false);
    connect(tableWidget_, &QTableWidget::itemSelectionChanged,
            this, &InsertVideoWidget::onItemSelectionChanged);
    connect(tableWidget_, &QTableWidget::cellDoubleClicked,
            this, [this](int row, int column) {
                if (column == 1) { // 双击文件名列播放
                    onPlayClicked();
                }
            });
    connect(tableWidget_, &QTableWidget::cellChanged,
            this, &InsertVideoWidget::onLoopCheckStateChanged);
    leftLayout->addWidget(tableWidget_, 3);

    // 底部状态栏
    auto* bottomLayout = new QHBoxLayout();
    selectedLabel_ = new QLabel(QString::fromUtf8("已选: 0 项"), this);
    selectedLabel_->setStyleSheet("color: #888888; font-size: 13px;");
    bottomLayout->addWidget(selectedLabel_);
    bottomLayout->addStretch();

    statusLabel_ = new QLabel(this);
    statusLabel_->setStyleSheet("color: #888888; font-size: 13px;");
    bottomLayout->addWidget(statusLabel_);
    leftLayout->addLayout(bottomLayout);

    // 按钮栏
    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();

    playButton_ = new QPushButton(QString::fromUtf8("▶ 预览播放"), this);
    playButton_->setEnabled(false);
    playButton_->setMinimumSize(100, 36);
    playButton_->setStyleSheet(
        "QPushButton { background-color: #3a5a8a; color: #ffffff; border-radius: 6px; padding: 8px 16px; }"
        "QPushButton:hover { background-color: #4a6aaa; }"
        "QPushButton:disabled { background-color: #2a3a4a; color: #666666; }"
    );
    connect(playButton_, &QPushButton::clicked, this, &InsertVideoWidget::onPlayClicked);
    buttonLayout->addWidget(playButton_);

    stopPreviewButton_ = new QPushButton(QString::fromUtf8("■ 停止预览"), this);
    stopPreviewButton_->setEnabled(false);
    stopPreviewButton_->setMinimumSize(100, 36);
    stopPreviewButton_->setStyleSheet(
        "QPushButton { background-color: #5a5a6a; color: #ffffff; border-radius: 6px; padding: 8px 16px; }"
        "QPushButton:hover { background-color: #6a6a7a; }"
        "QPushButton:disabled { background-color: #3a3a4a; color: #666666; }"
    );
    connect(stopPreviewButton_, &QPushButton::clicked, this, &InsertVideoWidget::onStopPreviewClicked);
    buttonLayout->addWidget(stopPreviewButton_);

    startInsertButton_ = new QPushButton(QString::fromUtf8("开始插播"), this);
    startInsertButton_->setEnabled(false);
    startInsertButton_->setMinimumSize(120, 36);
    // 与开始直播按钮一致的渐变样式
    startInsertButton_->setStyleSheet(
        "QPushButton { background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #4a6ef0, stop:1 #f05a6a); "
        "color: white; border-radius: 6px; padding: 8px 24px; font-weight: bold; }"
        "QPushButton:hover { background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #5a7eff, stop:1 #f16a7a); }"
        "QPushButton:disabled { background: #3a3a4a; color: #666666; }"
    );
    connect(startInsertButton_, &QPushButton::clicked, this, &InsertVideoWidget::onStartInsertClicked);
    buttonLayout->addWidget(startInsertButton_);

    leftLayout->addLayout(buttonLayout);

    // 右侧：预览区域
    auto* rightLayout = new QVBoxLayout();
    rightLayout->setSpacing(10);

    auto* previewTitle = new QLabel(QString::fromUtf8("预览"), this);
    previewTitle->setStyleSheet("font-size: 14px; font-weight: bold; color: #ffffff;");
    rightLayout->addWidget(previewTitle);

    // 预览窗口
    previewWidget_ = new QFrame(this);
    previewWidget_->setMinimumSize(400, 300);
    previewWidget_->setStyleSheet(
        "QFrame { background-color: #000000; border: 2px solid #3a3a3a; border-radius: 8px; }"
    );
    previewWidget_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // 设置 Qt 属性，确保 VLC 可以直接渲染到窗口
    previewWidget_->setAttribute(Qt::WA_OpaquePaintEvent, true);
    previewWidget_->setAttribute(Qt::WA_NoSystemBackground, true);
    previewWidget_->setAttribute(Qt::WA_DontCreateNativeAncestors, true);

    // 在预览窗口中放置一个标签用于显示
    previewLabel_ = new QLabel(previewWidget_);
    previewLabel_->setAlignment(Qt::AlignCenter);
    previewLabel_->setText(QString::fromUtf8("选择视频后点击预览播放"));
    previewLabel_->setStyleSheet("color: #666666; font-size: 14px;");
    previewLabel_->setGeometry(previewWidget_->rect());

    rightLayout->addWidget(previewWidget_, 1);

    contentLayout->addLayout(leftLayout, 2);  // 左侧占比2/3
    contentLayout->addLayout(rightLayout, 1); // 右侧占比1/3
    mainLayout->addLayout(contentLayout);

    // 设置对话框样式
    setStyleSheet("QDialog { background-color: #1a1a1a; }");
}

void InsertVideoWidget::setLiveInfo(const QString& sassUrl, const QString& userId, const QString& token, const QString& roomId) {
    sass_url_ = sassUrl;
    user_id_ = userId;
    token_ = token;

    // 直播间切换时重置初始化标志，确保 showEvent 重新拉取新房间的视频列表
    if (room_id_ != roomId) {
        room_id_ = roomId;
        is_initialized_ = false;
    }

    // 设置 InsertFileManager 的直播间信息
    InsertFileManager::instance()->setLiveInfo(sassUrl, userId, token);

    // VLC 播放器延迟初始化 - 在首次预览时再创建，避免阻塞主线程
    // 这样可以大大减少点击插播按钮时的等待时间
}

void InsertVideoWidget::refreshVideoList() {
    if (room_id_.isEmpty()) {
        statusLabel_->setText(QString::fromUtf8("直播间ID未设置"));
        return;
    }

    statusLabel_->setText(QString::fromUtf8("正在加载..."));
    last_error_message_.clear();
    InsertFileManager::instance()->refreshInsertFiles(room_id_);
}

void InsertVideoWidget::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);

    // 打开窗口时就后台预热 VLC，避免用户首次点击预览时卡顿
    startVlcPrewarmMonitoring();

    if (!is_initialized_) {
        is_initialized_ = true;
        refreshVideoList();
    }
}

void InsertVideoWidget::closeEvent(QCloseEvent *event)
{
    // 设置标志位阻止回调
    is_previewing_ = false;

    // 直接重置VLC播放器（异步方式）
    vlc_player_.reset();

    if (vlc_prewarm_timer_) {
        vlc_prewarm_timer_->stop();
        vlc_prewarm_timer_->deleteLater();
        vlc_prewarm_timer_ = nullptr;
    }

    event->accept();
    // 不调用QDialog::closeEvent避免重复处理
}

void InsertVideoWidget::keyPressEvent(QKeyEvent *event) {
    // 拦截回车键，防止在搜索框按回车时关闭对话框
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        // 如果焦点在搜索框，刷新列表而不是关闭对话框
        if (searchEdit_ && searchEdit_->hasFocus()) {
            event->accept();
            return; // 忽略回车键，让搜索框的搜索功能正常工作
        }
    }
    QDialog::keyPressEvent(event);
}

void InsertVideoWidget::startVlcPrewarmMonitoring() {
    if (vlc_prewarm_started_) {
        updateVlcPrewarmUi();
        return;
    }
    vlc_prewarm_started_ = true;

    VlcPlayer::prewarmAsync();
    updateVlcPrewarmUi();

    // 轮询就绪状态（libvlc 初始化在后台线程）
    vlc_prewarm_timer_ = new QTimer(this);
    vlc_prewarm_timer_->setInterval(200);
    connect(vlc_prewarm_timer_, &QTimer::timeout, this, [this]() {
        if (VlcPlayer::isPrewarmed()) {
            if (vlc_prewarm_timer_) {
                vlc_prewarm_timer_->stop();
                vlc_prewarm_timer_->deleteLater();
                vlc_prewarm_timer_ = nullptr;
            }
            updateVlcPrewarmUi();
            updateButtonStates();
        }
    });
    vlc_prewarm_timer_->start();
}

void InsertVideoWidget::updateVlcPrewarmUi() {
    if (!playButton_) return;
    const bool ready = VlcPlayer::isPrewarmed();
    playButton_->setText(ready ? QString::fromUtf8("预览播放") : QString::fromUtf8("预览初始化中..."));
    playButton_->setToolTip(ready ? QString() : QString::fromUtf8("播放器初始化中，请稍候..."));
}

void InsertVideoWidget::onCloseButtonClicked() {
    // 停止预览
    if (vlc_player_ && is_previewing_) {
        vlc_player_->stop();
        is_previewing_ = false;
    }

    // 设置标志位阻止回调
    is_previewing_ = false;

    // 重置VLC播放器
    vlc_player_.reset();

    if (vlc_prewarm_timer_) {
        vlc_prewarm_timer_->stop();
        vlc_prewarm_timer_->deleteLater();
        vlc_prewarm_timer_ = nullptr;
    }

    reject(); // 关闭对话框
}

void InsertVideoWidget::onRefreshClicked() {
    refreshVideoList();
}

void InsertVideoWidget::onPlayClicked() {
    auto item = InsertFileManager::instance()->getFile(selected_file_id_);
    if (!item) return;

    if (!VlcPlayer::isPrewarmed()) {
        statusLabel_->setText(QString::fromUtf8("播放器初始化中，请稍候再预览..."));
        updateButtonStates();
        return;
    }

    if (!item->isDownloaded()) {
        QMessageBox::warning(this, QString::fromUtf8("提示"),
            QString::fromUtf8("文件尚未下载完成，请等待下载完成后再播放"));
        return;
    }

    QString localPath = item->getLocalCachePath();

    // 延迟初始化 VLC 播放器（首次点击预览时初始化）
    if (!vlc_player_) {
        vlc_player_.reset(new VlcPlayer(previewWidget_));
        connect(vlc_player_.get(), &VlcPlayer::endReached, this, &InsertVideoWidget::onPreviewEndReached);
    }

    // 使用内嵌 VLC 播放器播放视频
    if (vlc_player_) {
        // 停止之前的预览
        if (is_previewing_) {
            vlc_player_->stop();
        }

        // 隐藏提示标签，让 VLC 渲染占据整个预览区域
        previewLabel_->hide();

        if (vlc_player_->openFile(localPath)) {
            vlc_player_->play();
            // 延迟刷新视频输出，确保窗口已完全初始化
            QTimer::singleShot(100, this, [this]() {
                if (vlc_player_) {
                    vlc_player_->refreshVideoOutput();
                }
            });
            is_previewing_ = true;
            updateButtonStates();
            LOG_INFO("InsertVideoWidget: Started preview playback for " + item->fileName.toStdString());
        } else {
            QMessageBox::warning(this, QString::fromUtf8("播放失败"),
                QString::fromUtf8("无法打开视频文件"));
            previewLabel_->show();
        }
    }
}

void InsertVideoWidget::onStopPreviewClicked() {
    if (vlc_player_ && is_previewing_) {
        vlc_player_->stop();
        is_previewing_ = false;
        previewLabel_->setText(QString::fromUtf8("选择视频后点击预览播放"));
        previewLabel_->setStyleSheet("color: #666666; font-size: 14px;");
        updateButtonStates();
        LOG_INFO("InsertVideoWidget: Stopped preview playback");
    }
}

void InsertVideoWidget::onStartInsertClicked() {
    auto item = InsertFileManager::instance()->getFile(selected_file_id_);
    if (!item) return;

    if (!item->isDownloaded()) {
        QMessageBox::warning(this, QString::fromUtf8("提示"),
            QString::fromUtf8("文件尚未下载完成，请等待下载完成后再开始插播"));
        return;
    }

    LOG_INFO("InsertVideoWidget: Start insert clicked for " + item->fileName.toStdString());

    // 停止预览
    if (vlc_player_ && is_previewing_) {
        // Avoid synchronous libvlc stop on the UI thread here. On Windows the
        // video output can block while tearing down D3D/child-window resources.
        vlc_player_->pause();
        is_previewing_ = false;
        previewLabel_->show();
    }

    // 获取循环播放设置
    bool loopEnabled = false;
    for (int row = 0; row < tableWidget_->rowCount(); row++) {
        auto* item = tableWidget_->item(row, 0);
        if (item && item->data(Qt::UserRole).toString() == selected_file_id_) {
            auto* loopItem = tableWidget_->item(row, 4);
            if (loopItem) {
                loopEnabled = (loopItem->checkState() == Qt::Checked);
            }
            break;
        }
    }

    // 更新 InsertFileItem 中的循环设置
    item->loopEnabled = loopEnabled;

    LOG_INFO("InsertVideoWidget: Emitting startInsertVideo for fileId=" + item->fileId.toStdString());
    emit startInsertVideo(item->fileId, item->fileName, loopEnabled);
    accept(); // 关闭对话框
}

void InsertVideoWidget::onSearchTextChanged(const QString& text) {
    updateVideoTable();
}

void InsertVideoWidget::onItemSelectionChanged() {
    auto selectedItems = tableWidget_->selectedItems();
    if (selectedItems.isEmpty()) {
        selected_file_id_.clear();
    } else {
        int row = selectedItems.first()->row();
        auto* item = tableWidget_->item(row, 0);
        if (item) {
            selected_file_id_ = item->data(Qt::UserRole).toString();
        }
    }

    updateButtonStates();
}

void InsertVideoWidget::onFilesUpdated() {
    updateVideoTable();
    statusLabel_->setText(QString::fromUtf8("共 %1 个视频").arg(InsertFileManager::instance()->getAllFiles().size()));
}

void InsertVideoWidget::onDownloadProgress(const QString& fileId, int percent) {
    auto* progressBar = progress_bars_.value(fileId, nullptr);
    if (progressBar) {
        progressBar->setValue(percent);
    }
}

void InsertVideoWidget::onDownloadFinished(const QString& fileId, bool success, const QString& message) {
    updateVideoTable(); // 刷新列表以更新状态
}

void InsertVideoWidget::onErrorOccurred(const QString& fileId, const QString& message) {
    LOG_ERROR("InsertVideoWidget: Error for file " + fileId.toStdString() + ": " + message.toStdString());
    statusLabel_->setText(message);

    if (message.isEmpty() || message == last_error_message_) {
        return;
    }

    last_error_message_ = message;
    QMessageBox::warning(this, QString::fromUtf8("错误"), message);
}

void InsertVideoWidget::onPreviewEndReached() {
    is_previewing_ = false;
    previewLabel_->setText(QString::fromUtf8("播放结束"));
    previewLabel_->setStyleSheet("color: #00aa00; font-size: 14px;");
    updateButtonStates();
}

void InsertVideoWidget::onLoopCheckStateChanged(int row, int column) {
    if (column != 4) return; // 只处理循环播放列

    auto* loopItem = tableWidget_->item(row, 4);
    if (!loopItem) return;

    auto* checkItem = tableWidget_->item(row, 0);
    if (!checkItem) return;

    QString fileId = checkItem->data(Qt::UserRole).toString();
    bool loopEnabled = (loopItem->checkState() == Qt::Checked);

    // 更新 InsertFileItem 中的循环设置
    auto item = InsertFileManager::instance()->getFile(fileId);
    if (item) {
        item->loopEnabled = loopEnabled;
        LOG_INFO("InsertVideoWidget: Loop playback for " + item->fileName.toStdString() +
                 " set to " + (loopEnabled ? "enabled" : "disabled"));
    }
}

void InsertVideoWidget::updateVideoTable() {
    tableWidget_->clearContents();
    progress_bars_.clear();

    auto files = InsertFileManager::instance()->getAllFiles();
    QString searchText = searchEdit_->text().toLower();

    int row = 0;
    for (const auto& file : files) {
        // 搜索过滤
        if (!searchText.isEmpty() && !file->fileName.toLower().contains(searchText)) {
            continue;
        }

        tableWidget_->setRowCount(row + 1);

        // 复选框列
        auto* checkItem = new QTableWidgetItem();
        checkItem->setFlags(checkItem->flags() | Qt::ItemIsUserCheckable);
        checkItem->setCheckState(Qt::Unchecked);
        checkItem->setData(Qt::UserRole, file->fileId);
        tableWidget_->setItem(row, 0, checkItem);

        // 文件列（可点击播放）
        auto* nameItem = new QTableWidgetItem(file->fileName);
        nameItem->setToolTip(file->fileName);
        nameItem->setForeground(QBrush(QColor("#4a6ef0"))); // 蓝色字体提示可点击
        tableWidget_->setItem(row, 1, nameItem);

        // 大小列
        QString sizeText = file->videoSize.trimmed();
        if (sizeText.isEmpty()) {
            qint64 fileSize = 0;
            QString localPath = file->getLocalCachePath();
            if (QFile::exists(localPath)) {
                fileSize = QFile(localPath).size();
            }
            sizeText = formatFileSize(fileSize);
        }
        auto* sizeItem = new QTableWidgetItem(sizeText);
        sizeItem->setTextAlignment(Qt::AlignCenter);
        tableWidget_->setItem(row, 2, sizeItem);

        // 时长列
        auto* durationItem = new QTableWidgetItem(formatDuration(file->durationMs));
        durationItem->setTextAlignment(Qt::AlignCenter);
        tableWidget_->setItem(row, 3, durationItem);

        // 循环播放列
        auto* loopItem = new QTableWidgetItem();
        loopItem->setFlags(loopItem->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEditable);
        loopItem->setCheckState(file->loopEnabled ? Qt::Checked : Qt::Unchecked);
        loopItem->setData(Qt::UserRole, file->fileId);
        loopItem->setTextAlignment(Qt::AlignCenter);
        tableWidget_->setItem(row, 4, loopItem);

        // 状态列
        QString statusText = insertFileStatusToString(file->status);
        QWidget* statusWidget = nullptr;

        if (file->status == InsertFileStatus::DOWNLOADING) {
            // 显示进度条
            auto* progressBar = new QProgressBar(this);
            progressBar->setRange(0, 100);
            progressBar->setValue(0);
            progressBar->setTextVisible(true);
            progressBar->setStyleSheet(
                "QProgressBar { border: 1px solid #444444; border-radius: 3px; text-align: center; "
                "background-color: #333333; color: #ffffff; }"
                "QProgressBar::chunk { background-color: #4a6ef0; border-radius: 2px; }"
            );
            progress_bars_[file->fileId] = progressBar;
            statusWidget = progressBar;
        } else {
            // 显示状态文本
            auto* label = new QLabel(statusText, this);
            label->setAlignment(Qt::AlignCenter);
            if (file->status == InsertFileStatus::DOWNLOAD_COMPLETED) {
                label->setStyleSheet("color: #00aa00;");
            } else if (file->status == InsertFileStatus::DOWNLOAD_FAILED) {
                label->setStyleSheet("color: #ff6a6a;");
            } else {
                label->setStyleSheet("color: #aaaaaa;");
            }
            statusWidget = label;
        }

        tableWidget_->setCellWidget(row, 5, statusWidget);

        row++;
    }

    selectedLabel_->setText(QString::fromUtf8("已选: %1 项").arg(row));
}

void InsertVideoWidget::updateButtonStates() {
    bool hasSelection = !selected_file_id_.isEmpty();
    const bool vlcReady = VlcPlayer::isPrewarmed();
    playButton_->setEnabled(hasSelection && vlcReady);
    stopPreviewButton_->setEnabled(is_previewing_);
    startInsertButton_->setEnabled(hasSelection);
    updateVlcPrewarmUi();

    if (hasSelection) {
        auto item = InsertFileManager::instance()->getFile(selected_file_id_);
        if (item && item->isDownloaded()) {
            startInsertButton_->setStyleSheet(
                "QPushButton { background-color: #4a6ef0; color: #ffffff; border: none; "
                "border-radius: 4px; padding: 10px 30px; font-weight: bold; }"
                "QPushButton:hover { background-color: #5a7eff; }"
            );
        } else {
            startInsertButton_->setStyleSheet(
                "QPushButton { background-color: #333333; color: #666666; border: none; "
                "border-radius: 4px; padding: 10px 30px; font-weight: bold; }"
            );
        }
    }
}

QString InsertVideoWidget::formatDuration(qint64 durationMs) const {
    if (durationMs <= 0) return "--:--";

    int seconds = durationMs / 1000;
    int minutes = seconds / 60;
    int hours = minutes / 60;

    seconds %= 60;
    minutes %= 60;

    if (hours > 0) {
        return QString("%1:%2:%3")
            .arg(hours, 2, 10, QChar('0'))
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'));
    } else {
        return QString("%1:%2")
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'));
    }
}

QString InsertVideoWidget::formatFileSize(qint64 bytes) const {
    if (bytes < 1024) {
        return QString("%1 B").arg(bytes);
    } else if (bytes < 1024 * 1024) {
        return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    } else if (bytes < 1024 * 1024 * 1024) {
        return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    } else {
        return QString("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
    }
}

QPixmap InsertVideoWidget::loadVideoThumbnail(const QString& coverUrl) const {
    // TODO: 实现缩略图加载
    // 可以下载封面图并缓存
    return QPixmap();
}
