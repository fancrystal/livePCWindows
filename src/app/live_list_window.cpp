#include "app/live_list_window.h"
#include "ui_live_list_window.h"
#include "http/client_service.h"
#include "http/live_item.h"
#include "app/config.h"
#include "common/log.h"
#include<QWindow>
#include <QJsonDocument>
#include <QMessageBox>
#include <QUrl>
#include <QPushButton>
#include <QGridLayout>
#include <QPixmap>
#include <QLabel>
#include <QEvent>
#include <QResizeEvent>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QGraphicsDropShadowEffect>
#include <QMouseEvent>
#include <QPointer>
#include <QPoint>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTextLayout>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <memory>

namespace live_assistant {

namespace {

QPixmap makeRoundedPixmap(const QPixmap& source, const QSize& targetSize, int radius)
{
    if (source.isNull() || !targetSize.isValid()) {
        return QPixmap();
    }

    QPixmap scaled = source.scaled(targetSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    QPixmap rounded(targetSize);
    rounded.fill(Qt::transparent);

    QPainter painter(&rounded);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, targetSize.width(), targetSize.height()), radius, radius);
    painter.setClipPath(path);

    const int x = (targetSize.width() - scaled.width()) / 2;
    const int y = (targetSize.height() - scaled.height()) / 2;
    painter.drawPixmap(x, y, scaled);
    return rounded;
}

QSize coverTargetSize()
{
    return QSize(264, 140);
}

QString coverCacheDir()
{
    const QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const QString dir = QDir::cleanPath(cacheRoot + "/live_list_covers");
    QDir().mkpath(dir);
    return dir;
}

QString coverCachePathForUrl(const QString& url)
{
    if (url.isEmpty()) {
        return QString();
    }

    const QByteArray hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex();
    QString suffix = QFileInfo(QUrl(url).path()).suffix().toLower();
    if (suffix.isEmpty()) {
        suffix = "img";
    }
    return QDir::cleanPath(coverCacheDir() + "/" + QString::fromLatin1(hash) + "." + suffix);
}

bool loadCachedCoverPixmap(const QString& url, const QSize& targetSize, QLabel* label)
{
    if (!label || url.isEmpty()) {
        return false;
    }

    const QString cachePath = coverCachePathForUrl(url);
    if (cachePath.isEmpty() || !QFileInfo::exists(cachePath)) {
        return false;
    }

    QPixmap pixmap(cachePath);
    if (pixmap.isNull()) {
        return false;
    }

    label->setPixmap(makeRoundedPixmap(pixmap, targetSize, 8));
    label->setScaledContents(false);
    return true;
}

void saveCoverBytesToCache(const QString& url, const QByteArray& bytes)
{
    const QString cachePath = coverCachePathForUrl(url);
    if (cachePath.isEmpty() || bytes.isEmpty()) {
        return;
    }

    QFile file(cachePath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(bytes);
    }
}

QString statusTextForItem(const LiveItem& item)
{
    return liveStatusToString(item.status);
}

QString orientationTextForItem(const LiveItem& item)
{
    return item.isPortraitMode() ? QStringLiteral("竖屏") : QStringLiteral("横屏");
}

QString coverUrlForItem(const LiveItem& item)
{
    if (item.isPortraitMode() && !item.verticalImageUrl.isEmpty()) {
        return item.verticalImageUrl;
    }
    if (!item.isPortraitMode() && !item.horizontalImageUrl.isEmpty()) {
        return item.horizontalImageUrl;
    }
    return item.liveShareImgUrl;
}

QString streamStateText(int streamState, bool hasUsableAddress)
{
    switch (streamState) {
    case 0:
        return QStringLiteral("● 未开始推流");
    case 1:
        return QStringLiteral("● 推流中");
    case 2:
        return QStringLiteral("● 推流已禁止");
    case 3:
        return QStringLiteral("● 直播已结束");
    case 4:
        return QStringLiteral("● 直播已过期");
    case -1:
        return QStringLiteral("● 推流状态获取失败");
    default:
        return hasUsableAddress ? QStringLiteral("● 推流地址可用") : QStringLiteral("● 推流地址不可用");
    }
}

QString streamStateStyle(int streamState, bool hasUsableAddress)
{
    QString color = "#9CA3AF";
    switch (streamState) {
    case 0:
        color = "#2AA7FF";
        break;
    case 1:
        color = "#22C55E";
        break;
    case 2:
        color = "#F59E0B";
        break;
    case 3:
    case 4:
        color = "#FF5A5A";
        break;
    case -1:
        color = "#9CA3AF";
        break;
    default:
        color = hasUsableAddress ? "#22C55E" : "#9CA3AF";
        break;
    }

    return QString("color: %1; font-size: 12px;").arg(color);
}

}

LiveListWindow::LiveListWindow(const QString& user_id, const QString& token, QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::LiveListWindow),
    user_id_(user_id),
    token_(token),
    current_page_(1),
    total_pages_(5),
    current_status_index_(0),
    current_search_keyword_(),
    dragging_(false),
    dragStartPos_(),
    combo_updating_(false),
    image_network_manager_(new QNetworkAccessManager(this)) {
    ui->setupUi(this);
    
    // 初始化服务器地址
    server_address_ = "ws://localhost:8080";
    
    // 设置窗口标题
    setWindowTitle("视频云直播 - 直播列表");
    
    // Remove default light background so the themed background image shows through
    setStyleSheet("QMainWindow { background-color: transparent; }");
    
    // Apply theme styling to header and controls to match design
    if (ui) {
        // header background transparent dark
        ui->headerWidget->setStyleSheet(
            "QWidget { background-color: rgba(20,12,16,0.5); }"
        );
        // search box: semi-transparent, rounded
        ui->searchLineEdit->setStyleSheet(
            "QLineEdit { background-color: rgba(255,255,255,0.06); color: #ffffff; border: 1px solid rgba(255,255,255,0.06); border-radius: 4px; padding: 6px; }"
        );
        ui->categoryComboBox->setStyleSheet(
            "QComboBox { background-color: rgba(255,255,255,0.04); color: #ffffff; border: 1px solid rgba(255,255,255,0.04); border-radius: 4px; padding: 4px; }"
            "QComboBox::drop-down { border: none; }"
            "QComboBox QAbstractItemView { background-color: rgba(40, 30, 50, 0.95); color: rgba(220, 200, 255, 0.9); selection-background-color: rgba(100, 80, 150, 0.6); selection-color: #ffffff; border: 1px solid rgba(100, 80, 150, 0.3); }"
        );
        // create button: gradient pill - 往左移，给关闭按钮留空间
        ui->createLiveButton->setStyleSheet(
            "QPushButton { background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #4a6ef0, stop:1 #f05a6a); color: white; border-radius: 6px; padding: 6px 12px; }"
        );
        // refresh button: gradient purple style (like settings button in main window)
        ui->refreshButton->setStyleSheet(
            "QPushButton { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #9C27B0, stop:1 #E040FB); color: white; border-radius: 4px; font-size: 14px; padding: 0px; }"
            "QPushButton:hover { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #AB47BC, stop:1 #EA80FC); }"
        );
        // pagination and grid spacing
        if (ui->gridLayout) {
            ui->gridLayout->setHorizontalSpacing(18);
            ui->gridLayout->setVerticalSpacing(18);
            ui->gridLayout->setContentsMargins(8,8,8,8);
        }
    }
    // Set background image for central widget using Qt resource and remove borders
    if (ui && ui->centralwidget) {
        ui->centralwidget->setContentsMargins(0,0,0,0);
        ui->centralwidget->setStyleSheet(
            "QWidget#centralwidget { background-image: url(:/images/livelist_back.png); background-repeat: no-repeat; background-position: center; background-attachment: fixed; border: none; }"
        );
    }

    // Add frameless control buttons (minimize / close) into headerWidget
    if (ui && ui->headerWidget) {
        // make window frameless and translucent
        setWindowFlag(Qt::FramelessWindowHint);
        setAttribute(Qt::WA_TranslucentBackground);

        // install event filter on header to support dragging
        ui->headerWidget->installEventFilter(this);

        // close button
        auto* closeBtn = new QPushButton(QString::fromUtf8("✕"), ui->headerWidget);
        closeBtn->setObjectName("winCloseBtn");
        closeBtn->setFixedSize(36, 28);
        closeBtn->setStyleSheet("QPushButton { background: rgba(255,255,255,0.06); color: white; border-radius: 4px; } QPushButton:hover { background: rgba(255,80,80,0.9); }");
        closeBtn->show();
        connect(closeBtn, &QPushButton::clicked, this, &LiveListWindow::close);

        // minimize button
        auto* miniBtn = new QPushButton(QString::fromUtf8("—"), ui->headerWidget);
        miniBtn->setObjectName("winMinBtn");
        miniBtn->setFixedSize(36, 28);
        miniBtn->setStyleSheet("QPushButton { background: rgba(255,255,255,0.02); color: white; border-radius: 4px; } QPushButton:hover { background: rgba(255,255,255,0.06); }");
        miniBtn->show();
        connect(miniBtn, &QPushButton::clicked, this, &LiveListWindow::showMinimized);

        // ensure buttons reposition if header resized
        ui->headerWidget->installEventFilter(this); 
    }

    // Use a single combined brand image to keep the icon and text aligned across DPI scales.
    if (ui && ui->headerWidget) {
        QPixmap brandPix(":/images/logo2.png");
        if (ui->logoLabel) {
            ui->logoLabel->setFixedSize(170, 32);
            ui->logoLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            if (!brandPix.isNull()) {
                ui->logoLabel->setPixmap(brandPix.scaled(ui->logoLabel->size(),
                                                          Qt::KeepAspectRatio,
                                                          Qt::SmoothTransformation));
            }
            ui->logoLabel->setText("");
            ui->logoLabel->setStyleSheet("background: transparent;");
        }
        if (ui->headerWidget->layout()) {
            QHBoxLayout* hl = qobject_cast<QHBoxLayout*>(ui->headerWidget->layout());
            if (hl) {
                hl->setAlignment(ui->logoLabel, Qt::AlignLeft | Qt::AlignVCenter);
            }
        }
    }
    // set initial positions for win buttons - 放在 createLiveButton 右边
    if (ui && ui->headerWidget) {
        QPushButton* closeBtn = ui->headerWidget->findChild<QPushButton*>("winCloseBtn");
        QPushButton* miniBtn = ui->headerWidget->findChild<QPushButton*>("winMinBtn");

        // 隐藏新建直播按钮（功能未实现）
        if (ui->createLiveButton) {
            ui->createLiveButton->hide();
        }

        // 从最右边往左计算位置
        int x = ui->headerWidget->width() - 8;

        if (closeBtn) {
            x -= closeBtn->width();
            closeBtn->move(x, 8);
            x -= 12;  // 按钮之间留12px间距
        }
        if (miniBtn) {
            x -= miniBtn->width();
            miniBtn->move(x, 8);
        }
    }
    // Hide statusbar/footer if present
    if (ui && ui->statusbar) {
        ui->statusbar->hide();
    }
    
    // 连接信号槽
    connect(ui->refreshButton, &QPushButton::clicked, this, &LiveListWindow::on_refreshButton_clicked);
    connect(ui->createLiveButton, &QPushButton::clicked, this, &LiveListWindow::on_createLiveButton_clicked);
    connect(ui->searchButton, &QPushButton::clicked, this, &LiveListWindow::on_searchButton_clicked);
    // 搜索框回车键触发搜索
    connect(ui->searchLineEdit, &QLineEdit::returnPressed, this, &LiveListWindow::on_searchButton_clicked);
    connect(ui->prevPageButton, &QPushButton::clicked, this, &LiveListWindow::on_prevPageButton_clicked);
    connect(ui->nextPageButton, &QPushButton::clicked, this, &LiveListWindow::on_nextPageButton_clicked);
    connect(ui->categoryComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &LiveListWindow::on_categoryComboBox_currentIndexChanged);
    
    // 设置页按钮连接
    QList<QPushButton*> page_buttons = {
        ui->page1Button, ui->page2Button, ui->page3Button, ui->page4Button, ui->page5Button
    };
    
    for (QPushButton* button : page_buttons) {
        connect(button, &QPushButton::clicked, this, &LiveListWindow::on_page_button_clicked);
    }
    // Style pagination buttons
    for (QPushButton* button : page_buttons) {
        button->setStyleSheet("QPushButton { background-color: transparent; color: rgba(255,255,255,0.8); border: 1px solid rgba(255,255,255,0.04); border-radius: 6px; padding: 4px 8px; }");
    }
    // mark first page active
    if (ui->page1Button) ui->page1Button->setStyleSheet("QPushButton { background-color: #4a6ef0; color: white; border-radius: 6px; padding: 4px 8px; }");
    // center pagination widget
    if (ui->verticalLayout) {
        ui->verticalLayout->setAlignment(ui->paginationWidget, Qt::AlignHCenter);
        ui->paginationWidget->setStyleSheet("background: transparent;"); 
    }
    
    // 初始化直播列表（加载数据会调用 setup_live_list）
    load_live_list();
    
    LOG_INFO("LiveListWindow created");

    // DPI 适配：监听屏幕 DPI 变化
    connect(windowHandle(), &QWindow::screenChanged, this, [this](QScreen* screen) {
        if (screen) {
            LOG_INFO("LiveListWindow: Screen changed, DPI: " + std::to_string(screen->logicalDotsPerInch()));
            this->updateGeometry();
        }
    });

    if (windowHandle() && windowHandle()->screen()) {
        connect(windowHandle()->screen(), &QScreen::logicalDotsPerInchChanged, this, [this](qreal dpi) {
            LOG_INFO("LiveListWindow: DPI changed to: " + std::to_string(dpi));
            this->updateGeometry();
        });
    }

    // QML integration removed — keeping original QWidget-based UI
}

LiveListWindow::~LiveListWindow() {
    LOG_INFO("LiveListWindow destroyed");
    delete ui;
}

void LiveListWindow::setup_live_list() {
    // 记录当前窗口位置，防止布局更新导致窗口偏移
    QPoint windowPos = this->pos();

    // 清空当前列表
    if (ui && ui->gridLayout) {
        QLayoutItem* item;
        while ((item = ui->gridLayout->takeAt(0)) != nullptr) {
            if (item->widget()) {
                item->widget()->hide();
                delete item->widget();
            }
            delete item;
        }

        // 重新设置布局参数
        ui->gridLayout->setHorizontalSpacing(18);
        ui->gridLayout->setVerticalSpacing(18);
        ui->gridLayout->setContentsMargins(8, 8, 8, 8);
        ui->gridLayout->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    }
    
    // Pagination: display subset of live_list_ per page
    const int page_size = 8;
    int total_items = live_list_.size();
    if (total_items == 0) return;
    total_pages_ = (total_items + page_size - 1) / page_size;
    if (current_page_ < 1) current_page_ = 1;
    if (current_page_ > total_pages_) current_page_ = total_pages_;
    int start = (current_page_ - 1) * page_size;
    int end = qMin(start + page_size, total_items);

    ensure_stream_status_for_page(start, end);

    for (int idx = start; idx < end; ++idx) {
        int i = idx;
        int pageSlot = idx - start;
        QWidget* card = new QWidget(this);
        card->setFixedSize(280, 250);
        card->setStyleSheet("background-color: rgba(8,6,8,0.6); border: 1px solid rgba(255,255,255,0.06); border-radius: 8px;");
        const QSize targetCoverSize = coverTargetSize();

        QVBoxLayout* vbox = new QVBoxLayout(card);
        vbox->setContentsMargins(8, 8, 8, 8);
        vbox->setSpacing(6);

        QLabel* thumb = new QLabel(card);
        thumb->setFixedHeight(140);
        thumb->setFixedWidth(targetCoverSize.width());
        thumb->setAlignment(Qt::AlignCenter);
        QPixmap pixmap(":/images/Frame back.png");
        if (!pixmap.isNull()) {
            thumb->setPixmap(makeRoundedPixmap(pixmap, targetCoverSize, 8));
            thumb->setScaledContents(false);
        } else {
            thumb->setStyleSheet("background-color: rgba(255,255,255,0.06); border-radius:4px;");
        }
        vbox->addWidget(thumb);

        // if live_list_ has data, use it
        QString titleText = QString("测试直播间%1").arg(i+1);
        QString metaText = QString("房间号: %1   %2").arg(100000 + i).arg("2026-01-04 14:00:00");
        QString typeText = QString("画面: 未知");
        QString countText = QString("预约: 0  观看: 0");
        QString status = QStringLiteral("未知");

        QString streamStatusText;
        QString streamStatusStyle = QStringLiteral("color: #9CA3AF; font-size: 12px;");

        if (i < current_live_items_.size()) {
            const LiveItem& item = current_live_items_.at(i);
            const QString roomNumber = item.roomNumber.isEmpty() ? item.liveId : item.roomNumber;
            const QString timeText = item.startTime.isValid()
                ? item.startTime.toString("yyyy-MM-dd HH:mm:ss")
                : item.createTime.toString("yyyy-MM-dd HH:mm:ss");

            titleText = item.title;
            metaText = QString("房间号: %1   %2").arg(roomNumber, timeText);
            typeText = QString("画面: %1").arg(orientationTextForItem(item));
            countText = QString("预约: %1  观看: %2").arg(item.reserveCount).arg(item.viewCount);
            status = statusTextForItem(item);

            if (stream_info_cache_.contains(item.liveId)) {
                const StreamNameInfo streamInfo = stream_info_cache_.value(item.liveId);
                const bool hasUsableAddress = !streamInfo.obsServer.isEmpty() || !streamInfo.obsStreamKey.isEmpty();
                streamStatusText = streamStateText(streamInfo.streamState, hasUsableAddress);
                streamStatusStyle = streamStateStyle(streamInfo.streamState, hasUsableAddress);
            }

            const QString coverUrl = coverUrlForItem(item);
            if (!coverUrl.isEmpty() && image_network_manager_) {
                if (!loadCachedCoverPixmap(coverUrl, targetCoverSize, thumb)) {
                    QPointer<QLabel> thumbGuard(thumb);
                    QNetworkReply* reply = image_network_manager_->get(QNetworkRequest(QUrl(coverUrl)));
                    connect(reply, &QNetworkReply::finished, this, [reply, thumbGuard, coverUrl, targetCoverSize]() {
                        if (!thumbGuard) {
                            reply->deleteLater();
                            return;
                        }

                        std::unique_ptr<QNetworkReply, void(*)(QNetworkReply*)> replyGuard(reply, [](QNetworkReply* r) {
                            if (r) {
                                r->deleteLater();
                            }
                        });

                        if (reply->error() != QNetworkReply::NoError) {
                            return;
                        }

                        const QByteArray bytes = reply->readAll();
                        QPixmap remotePixmap;
                        if (!remotePixmap.loadFromData(bytes)) {
                            return;
                        }

                        saveCoverBytesToCache(coverUrl, bytes);
                        thumbGuard->setPixmap(makeRoundedPixmap(remotePixmap, targetCoverSize, 8));
                        thumbGuard->setScaledContents(false);
                    });
                }
            }
        }

        QLabel* title = new QLabel(card);
        title->setStyleSheet("color: white; font-size: 14px; font-weight: bold;");
        title->setWordWrap(true);
        title->setFixedHeight(36); // approx two lines
        // compute two-line elided text using QTextLayout
        {
            int availW = card->width() - 16; // account for vbox margins
            QFontMetrics fm(title->font());
            QTextLayout layout(titleText, title->font());
            layout.beginLayout();
            QList<QTextLine> lines;
            while (true) {
                QTextLine line = layout.createLine();
                if (!line.isValid()) break;
                line.setLineWidth(availW);
                lines.append(line);
                if (lines.size() >= 2) break;
            }
            layout.endLayout();
            QString display;
            if (lines.isEmpty()) {
                display = titleText;
            } else if (lines.size() == 1) {
                int start = lines[0].textStart();
                int len = lines[0].textLength();
                display = titleText.mid(start, len);
            } else {
                int start0 = lines[0].textStart();
                int len0 = lines[0].textLength();
                int start1 = lines[1].textStart();
                QString part0 = titleText.mid(start0, len0);
                QString part1 = titleText.mid(start1);
                part1 = fm.elidedText(part1, Qt::ElideRight, availW);
                display = part0 + "\n" + part1;
            }
            title->setText(display);
        }
        vbox->addWidget(title);

        QLabel* meta = new QLabel(metaText, card);
        meta->setStyleSheet("color: rgba(255,255,255,0.7); font-size: 11px;");
        vbox->addWidget(meta);

        // 添加类型信息
        QLabel* typeLabel = new QLabel(typeText, card);
        typeLabel->setStyleSheet("color: rgba(255,255,255,0.6); font-size: 10px;");
        vbox->addWidget(typeLabel);

        // 添加预约和观看人数
        QLabel* countLabel = new QLabel(countText, card);
        countLabel->setStyleSheet("color: rgba(255,255,255,0.6); font-size: 10px;");
        vbox->addWidget(countLabel);

        QLabel* streamStatusLabel = new QLabel(streamStatusText, card);
        streamStatusLabel->setAlignment(Qt::AlignCenter);
        streamStatusLabel->setFixedHeight(22);
        streamStatusLabel->setStyleSheet(streamStatusStyle);
        vbox->addWidget(streamStatusLabel);

        // Overlay transparent button to handle clicks
        QPushButton* overlay = new QPushButton(card);
        overlay->setFlat(true);
        overlay->setStyleSheet(
            "QPushButton { background: transparent; border: none; }"
            "QPushButton:hover { border: 1px solid rgba(74,110,240,0.8); }"
        );
        overlay->setGeometry(0, 0, card->width(), card->height());
        overlay->raise();
        // 按值捕获 idx 以避免循环变量问题
        connect(overlay, &QPushButton::clicked, [this, idx]() { on_live_item_clicked(idx); });

        // small icon top-left
        QLabel* icon = new QLabel(card);
        QPixmap iconPix(":/images/logo_new.png");
        if (!iconPix.isNull()) {
            icon->setPixmap(iconPix.scaled(18,18, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } else {
            icon->setText("●");
            icon->setStyleSheet("color: rgba(255,255,255,0.9);");
        }
        icon->adjustSize();
        icon->move(10, 10);
        icon->raise();

        // Status badge (top-right)
        QLabel* badge = new QLabel(card);
        badge->setText(status);
        if (status == QStringLiteral("直播中")) {
            badge->setStyleSheet("background: rgba(34,197,94,0.18); color: white; padding: 4px 6px; border-radius: 10px; font-size:11px;");
        } else {
            badge->setStyleSheet("background: rgba(255,255,255,0.06); color: white; padding: 4px 6px; border-radius: 10px; font-size:11px;");
        }
        badge->adjustSize();
        badge->move(card->width() - badge->width() - 10, 10);
        badge->raise();

        // install base shadow and event filter for hover shadow
        QGraphicsDropShadowEffect* baseEffect = new QGraphicsDropShadowEffect(card);
        baseEffect->setBlurRadius(8);
        baseEffect->setOffset(0,2);
        baseEffect->setColor(QColor(0,0,0,80));
        card->setGraphicsEffect(baseEffect);
        card->installEventFilter(this);

        int row = pageSlot / 4;
        int col = pageSlot % 4;
        ui->gridLayout->addWidget(card, row, col);
    }
    // fill remaining slots with placeholders so layout keeps height (2x4)
    int itemsRendered = end - start;
    for (int s = itemsRendered; s < page_size; ++s) {
        QWidget* placeholder = new QWidget(this);
        placeholder->setFixedSize(280, 250);
        placeholder->setStyleSheet("background: transparent; border: none;");
        int row = s / 4;
        int col = s % 4;
        ui->gridLayout->addWidget(placeholder, row, col);
    }

    // 更新分页按钮状态
    update_pagination();

    // 恢复窗口位置（防止 Qt 内部布局计算导致窗口偏移）
    if (this->pos() != windowPos) {
        this->move(windowPos);
    }

    LOG_INFO("Live list setup completed");
}

void LiveListWindow::load_live_list() {
    stream_info_cache_.clear();
    LOG_INFO("开始加载直播列表...");

    // 清除搜索关键字和搜索框
    current_search_keyword_.clear();
    ui->searchLineEdit->clear();

    // 获取服务器配置
    QString liveUrl = ::ConfigManager::instance().getLiveUrl();

    // 获取当前选中状态对应的roomState
    int roomState = getCurrentRoomState();

    // 使用 ClientService 获取直播列表
    ClientService* client = ClientService::instance();
    QList<LiveItem> liveList;
    int totalCount = 0;
    QString errMsg;

    bool success = client->getLiveList(
        liveUrl,
        user_id_,
        token_,
        1,          // pageNum
        100,        // pageSize
        0,          // liveType: 0=全部
        roomState,  // liveStreamStatus: 根据UI选择
        1,          // reviewStatus: 1=审核通过
        liveList,
        totalCount,
        errMsg
    );

    if (!success) {
        LOG_WARNING(QString("获取直播列表失败: %1").arg(errMsg).toStdString());
        QMessageBox::warning(this, "错误", QString("获取直播列表失败: %1").arg(errMsg));
        return;
    }

    LOG_INFO(QString("成功获取直播列表: %1条记录").arg(liveList.size()).toStdString());

    // 保存完整的 LiveItem 列表
    full_live_items_ = liveList;

    // 转换为QJsonArray格式
    QJsonArray formattedList;
    for (const LiveItem& item : liveList) {
        QJsonObject formatted;
        formatted["id"] = item.liveId;
        formatted["name"] = item.title;
        formatted["startTime"] = item.startTime.isValid()
            ? item.startTime.toString("yyyy-MM-dd HH:mm:ss")
            : item.createTime.toString("yyyy-MM-dd HH:mm:ss");
        formatted["roomNumber"] = item.roomNumber;
        formatted["orientation"] = orientationTextForItem(item);
        formatted["type"] = item.roomType;
        formatted["coverUrl"] = coverUrlForItem(item);
        formatted["roomState"] = item.roomState;

        // 状态映射
        formatted["status"] = statusTextForItem(item);

        formatted["viewCount"] = item.viewCount;
        formatted["reserveCount"] = item.reserveCount;

        formattedList.append(formatted);
    }

    on_live_list_received(formattedList);
}

void LiveListWindow::add_live_item(const QJsonObject& live_info) {
    QString live_id = live_info["id"].toString();
    QString live_name = live_info["name"].toString();
    QString live_status = live_info["status"].toString();
    QString start_time = live_info["startTime"].toString();
    
    LOG_INFO("Adding live item: " + live_name.toStdString());
}

void LiveListWindow::on_live_item_clicked(int index) {
    // 检查索引是否有效
    if (index < 0 || index >= live_list_.size()) {
        LOG_WARNING("Invalid live item index: " + QString::number(index).toStdString());
        return;
    }

    // 检查 LiveItem 索引是否有效
    if (index < 0 || index >= current_live_items_.size()) {
        LOG_WARNING("Invalid LiveItem index: " + QString::number(index).toStdString());
        return;
    }

    // 从 live_list_ 中获取真实的直播间 ID
    QJsonObject live_obj = live_list_.at(index).toObject();
    QString live_id = live_obj.value("id").toString();

    if (live_id.isEmpty()) {
        LOG_WARNING("Live ID is empty for index: " + QString::number(index).toStdString());
        return;
    }

    // 从 current_live_items_ 中获取对应的 LiveItem（索引对应）
    const LiveItem& liveItem = current_live_items_[index];

    LOG_INFO(QString("Live item clicked: index=%1, live_id=%2, title=%3")
        .arg(index).arg(live_id).arg(liveItem.title).toStdString());

    // 发射直播选中信号（传递真实的直播间ID和完整的LiveItem）
    emit live_selected(live_id, liveItem);

    // 隐藏当前窗口（而不是关闭，这样返回时可以快速显示）
    hide();
}

void LiveListWindow::on_live_list_received(const QJsonArray& live_list) {
    full_live_list_ = live_list;  // 保存完整列表JSON

    // 如果有搜索关键字，进行过滤；否则显示全部
    if (!current_search_keyword_.isEmpty()) {
        filter_live_list(current_search_keyword_);
    } else {
        live_list_ = full_live_list_;
        current_live_items_ = full_live_items_;  // 同时更新LiveItem列表
    }

    current_page_ = 1;  // 重置到第一页
    setup_live_list();
    LOG_INFO("Live list received: " + QString::number(full_live_list_.size()).toStdString() + " items, displayed: " + QString::number(live_list_.size()).toStdString() + " items");
}

void LiveListWindow::on_refreshButton_clicked() {
    LOG_INFO("Refresh button clicked");
    load_live_list();
}

// QML-related methods removed

void LiveListWindow::on_createLiveButton_clicked() {
    LOG_INFO("Create live button clicked");
    QMessageBox::information(this, "提示", "新建直播功能开发中...");
}

void LiveListWindow::on_searchButton_clicked() {
    QString search_text = ui->searchLineEdit->text().trimmed();
    LOG_INFO("Search button clicked: " + search_text.toStdString());

    current_search_keyword_ = search_text;

    if (search_text.isEmpty()) {
        // 如果搜索框为空，显示完整列表
        live_list_ = full_live_list_;
    } else {
        // 过滤列表
        filter_live_list(search_text);
    }

    // 重置到第一页并更新显示
    current_page_ = 1;
    setup_live_list();
    LOG_INFO(QString("Search completed. Found %1 items").arg(live_list_.size()).toStdString());
}

// 根据关键字过滤直播列表（不完全匹配）
void LiveListWindow::filter_live_list(const QString& keyword) {
    live_list_ = QJsonArray();
    current_live_items_.clear();

    // 同时过滤 LiveItem 列表
    for (int i = 0; i < full_live_list_.size(); ++i) {
        QJsonObject obj = full_live_list_.at(i).toObject();
        QString name = obj.value("name").toString();
        QString id = obj.value("id").toString();
        QString roomNumber = obj.value("roomNumber").toString();

        // 不完全匹配：只要包含关键字即可
        if (name.contains(keyword, Qt::CaseInsensitive) ||
            id.contains(keyword, Qt::CaseInsensitive) ||
            roomNumber.contains(keyword, Qt::CaseInsensitive)) {
            live_list_.append(obj);
            // 同时保存对应的 LiveItem（确保索引对应）
            if (i < full_live_items_.size()) {
                current_live_items_.append(full_live_items_[i]);
            }
        }
    }

    LOG_INFO(QString("Filter by '%1': %2 -> %3 items")
        .arg(keyword)
        .arg(full_live_list_.size())
        .arg(live_list_.size())
        .toStdString());
}

void LiveListWindow::ensure_stream_status_for_page(int start, int end) {
    const QString liveUrl = ::ConfigManager::instance().getLiveUrl();
    if (liveUrl.isEmpty()) {
        return;
    }

    const int safeStart = qMax(0, start);
    const int safeEnd = qMin(end, current_live_items_.size());
    for (int i = safeStart; i < safeEnd; ++i) {
        const LiveItem& item = current_live_items_.at(i);
        if (item.liveId.isEmpty() || stream_info_cache_.contains(item.liveId)) {
            continue;
        }

        StreamNameInfo streamInfo;
        QString errMsg;
        if (ClientService::instance()->getStreamName(liveUrl, token_, item.liveId, streamInfo, errMsg)) {
            stream_info_cache_.insert(item.liveId, streamInfo);
        } else {
            StreamNameInfo failedInfo;
            failedInfo.streamState = -1;
            stream_info_cache_.insert(item.liveId, failedInfo);
            LOG_WARNING(QString("Failed to get StreamName for roomInfoId %1: %2")
                        .arg(item.liveId)
                        .arg(errMsg)
                        .toStdString());
        }
    }
}

void LiveListWindow::on_prevPageButton_clicked() {
    if (current_page_ > 1) {
        current_page_--;
        setup_live_list();
        LOG_INFO("Prev page: " + QString::number(current_page_).toStdString());
    }
}

void LiveListWindow::on_nextPageButton_clicked() {
    if (current_page_ < total_pages_) {
        current_page_++;
        setup_live_list();
        LOG_INFO("Next page: " + QString::number(current_page_).toStdString());
    }
}

void LiveListWindow::update_pagination() {
    const int page_size = 8;
    int total_items = live_list_.size();
    total_pages_ = (total_items + page_size - 1) / page_size;

    bool show_pagination = total_pages_ > 1;

    ui->prevPageButton->setVisible(show_pagination);
    ui->nextPageButton->setVisible(show_pagination);
    ui->ellipsisLabel->setVisible(false);
    ui->lastPageButton->setVisible(false);

    QList<QPushButton*> page_buttons = {
        ui->page1Button, ui->page2Button, ui->page3Button, ui->page4Button, ui->page5Button
    };

    if (!show_pagination) {
        for (auto b : page_buttons) b->setVisible(false);
        ui->ellipsisLabel->setVisible(false);
        ui->lastPageButton->setVisible(false);
        return;
    }

    // sliding window of page buttons centered around current_page_
    int maxButtons = 5;
    int startPage = 1;
    if (total_pages_ > maxButtons) {
        startPage = current_page_ - maxButtons/2;
        if (startPage < 1) startPage = 1;
        if (startPage + maxButtons - 1 > total_pages_) startPage = total_pages_ - maxButtons + 1;
    }

    for (int i = 0; i < maxButtons; ++i) {
        int pageNum = startPage + i;
        if (pageNum <= total_pages_) {
            page_buttons[i]->setVisible(true);
            page_buttons[i]->setText(QString::number(pageNum));
            if (pageNum == current_page_) {
                page_buttons[i]->setStyleSheet("QPushButton { background-color: #4a6ef0; color: white; border-radius: 6px; padding: 4px 8px; }");
            } else {
                page_buttons[i]->setStyleSheet("QPushButton { background-color: rgba(0,0,0,0.35); color: white; border: 1px solid rgba(255,255,255,0.06); border-radius: 6px; padding: 4px 8px; }");
            }
        } else {
            page_buttons[i]->setVisible(false);
        }
    }

    if (total_pages_ > maxButtons) {
        ui->ellipsisLabel->setVisible(true);
        ui->lastPageButton->setVisible(true);
        ui->lastPageButton->setText(QString::number(total_pages_));
        ui->lastPageButton->setStyleSheet("QPushButton { background-color: rgba(0,0,0,0.35); color: white; border: 1px solid rgba(255,255,255,0.06); border-radius: 6px; padding: 4px 8px; }");
    }
}

void LiveListWindow::on_page_button_clicked() {
    QPushButton* sender_button = qobject_cast<QPushButton*>(sender());
    if (sender_button) {
        int page = sender_button->text().toInt();
        if (page != current_page_ && page > 0 && page <= total_pages_) {
            current_page_ = page;
            // refresh page view and pagination styling
            setup_live_list();
            LOG_INFO("Page button clicked: " + QString::number(current_page_).toStdString());
            
            // pagination styling updated by setup_live_list()/update_pagination()
        }
    }
}

bool LiveListWindow::eventFilter(QObject* watched, QEvent* event) {
    QWidget* w = qobject_cast<QWidget*>(watched);
    if (!w) return QMainWindow::eventFilter(watched, event);
    // headerWidget: handle mouse drag for frameless window and resize repositioning
    if (ui && watched == ui->headerWidget) {
        if (event->type() == QEvent::MouseButtonPress) {
            QMouseEvent* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                dragging_ = true;
                dragStartPos_ = me->globalPos() - frameGeometry().topLeft();
                return true;
            }
        } else if (event->type() == QEvent::MouseMove) {
            if (dragging_) {
                QMouseEvent* me = static_cast<QMouseEvent*>(event);
                move(me->globalPos() - dragStartPos_);
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            dragging_ = false;
            return true;
        } else if (event->type() == QEvent::Resize) {
            // reposition control buttons to top-right corner
            QPushButton* closeBtn = ui->headerWidget->findChild<QPushButton*>("winCloseBtn");
            QPushButton* miniBtn = ui->headerWidget->findChild<QPushButton*>("winMinBtn");

            // 隐藏新建直播按钮（功能未实现）
            if (ui->createLiveButton) {
                ui->createLiveButton->hide();
            }

            int x = ui->headerWidget->width() - 8;

            if (closeBtn) {
                x -= closeBtn->width();
                closeBtn->move(x, 8);
                x -= 12;
            }
            if (miniBtn) {
                x -= miniBtn->width();
                miniBtn->move(x, 8);
            }
            return false;
        }
    }

    // card hover shadow (only for other widgets that aren't header)
    if (event->type() == QEvent::Enter) {
        auto* effect = qobject_cast<QGraphicsDropShadowEffect*>(w->graphicsEffect());
        if (effect) {
            effect->setBlurRadius(24);
            effect->setOffset(0, 10);
            effect->setColor(QColor(0,0,0,200));
        } else {
            auto* e2 = new QGraphicsDropShadowEffect(w);
            e2->setBlurRadius(24);
            e2->setOffset(0, 10);
            e2->setColor(QColor(0,0,0,200));
            w->setGraphicsEffect(e2);
        }
        return false;
    } else if (event->type() == QEvent::Leave) {
        auto* effect = qobject_cast<QGraphicsDropShadowEffect*>(w->graphicsEffect());
        if (effect) {
            effect->setBlurRadius(8);
            effect->setOffset(0, 2);
            effect->setColor(QColor(0,0,0,80));
        }
        return false;
    }

    return QMainWindow::eventFilter(watched, event);
}

// 根据当前选中状态返回对应的roomState值
// 0=待开播, 1=直播中, 2=已结束
int LiveListWindow::getCurrentRoomState() const {
    // categoryComboBox 索引: 0=待开播(1), 1=直播中(2), 2=已结束(3)
    return current_status_index_ + 1;
}

// 状态筛选下拉框切换
void LiveListWindow::on_categoryComboBox_currentIndexChanged(int index) {
    if (index < 0 || index > 2) return;
    if (combo_updating_) return;  // 防止 setup_live_list 期间的布局事件触发重复请求

    combo_updating_ = true;
    current_status_index_ = index;
    current_page_ = 1;  // 切换状态时重置到第一页
    load_live_list();
    combo_updating_ = false;

    QString statusNames[] = {"待开播", "直播中", "已结束"};
    LOG_INFO(QString("切换状态筛选: %1").arg(statusNames[index]).toStdString());
}

void LiveListWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);

    // 确保 gridLayout 在窗口大小改变后正确更新
    if (ui && ui->gridLayout) {
        ui->gridLayout->invalidate();
        ui->gridLayout->activate();
    }
}

} // namespace live_assistant
