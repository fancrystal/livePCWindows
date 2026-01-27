#include "app/live_list_window.h"
#include "ui_live_list_window.h"
#include "common/log.h"
#include <QJsonDocument>
#include <QMessageBox>
#include <QUrl>
#include <QPushButton>
#include <QGridLayout>
#include <QPixmap>
#include <QLabel>
#include <QEvent>
#include <QGraphicsDropShadowEffect>
#include <QPushButton>
#include <QMouseEvent>
#include <QPoint>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QLabel>

namespace live_assistant {

LiveListWindow::LiveListWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::LiveListWindow),
    network_manager_(nullptr),
    current_page_(1),
    total_pages_(5) {
    ui->setupUi(this);
    
    // 初始化服务器地址
    server_address_ = "ws://localhost:8080";
    
    // 创建NetworkManager实例
    network_manager_ = new NetworkManager(this);
    
    // 设置窗口标题
    setWindowTitle("启点点直播 - 直播列表");
    
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
        );
        // create button: gradient pill
        ui->createLiveButton->setStyleSheet(
            "QPushButton { background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #4a6ef0, stop:1 #f05a6a); color: white; border-radius: 6px; padding: 6px 12px; }"
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

    // add logo and gradient title (use existing ui->logoLabel and insert title widget into the header layout)
    if (ui && ui->headerWidget) {
        // set ui->logoLabel to use Frame_icon.png; always clear original text
        QPixmap iconPix2(":/images/Frame_icon.png");
        if (ui->logoLabel) {
            if (!iconPix2.isNull()) {
                // slightly smaller to match target header compactness
                ui->logoLabel->setPixmap(iconPix2.scaled(32,32, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                ui->logoLabel->setFixedSize(32,32);
            }
            ui->logoLabel->setText("");
            ui->logoLabel->setStyleSheet("background: transparent;");
        }

        // gradient text '直播伴侣' + suffix, inserted into header layout after logoLabel
        QString gradText = QString::fromUtf8("直播伴侣");
        QString suffixText = QString::fromUtf8(" · 启点点");
        QFont titleFont = ui->headerWidget->font();
        titleFont.setPointSize(14);
        titleFont.setBold(true);

        QPainterPath path;
        path.addText(0, 0, titleFont, gradText);
        QRectF br = path.boundingRect();
        QPixmap titlePixmap(int(br.width()) + 4, int(br.height()) + 4);
        titlePixmap.fill(Qt::transparent);
        {
            QPainter p(&titlePixmap);
            p.setRenderHint(QPainter::Antialiasing);
            p.translate(-br.left(), -br.top());
            QLinearGradient lg(0, 0, br.width(), 0);
            lg.setColorAt(0.0, QColor(255, 106, 106));
            lg.setColorAt(1.0, QColor(74, 110, 240));
            p.fillPath(path, QBrush(lg));
        }

        // container widget to hold title and suffix
        QWidget* titleContainer = new QWidget(ui->headerWidget);
        titleContainer->setObjectName("titleContainer");
        titleContainer->setAttribute(Qt::WA_TranslucentBackground);
        titleContainer->setAutoFillBackground(false);
        titleContainer->setStyleSheet("background: transparent;");
        QHBoxLayout* tl = new QHBoxLayout(titleContainer);
        tl->setContentsMargins(0, -8, 0, 0); // nudge upward slightly more
        tl->setSpacing(6);
        QLabel* titleLabel = new QLabel(titleContainer);
        titleLabel->setPixmap(titlePixmap);
        titleLabel->setFixedSize(titlePixmap.size());
        QLabel* suffixLabel = new QLabel(suffixText, titleContainer);
        titleLabel->setAttribute(Qt::WA_TranslucentBackground);
        titleLabel->setStyleSheet("background: transparent;");
        suffixLabel->setAttribute(Qt::WA_TranslucentBackground);
        suffixLabel->setStyleSheet("background: transparent;");
        QFont sufFont = titleFont;
        sufFont.setBold(true);
        sufFont.setItalic(true);
        sufFont.setPointSize(11);
        suffixLabel->setFont(sufFont);
        suffixLabel->setStyleSheet("color: rgba(255,255,255,0.95);");
        tl->addWidget(titleLabel);
        tl->addWidget(suffixLabel);
        titleContainer->setLayout(tl);

        // insert into header layout after the existing logoLabel (index 1)
        if (ui->headerWidget->layout()) {
            QHBoxLayout* hl = qobject_cast<QHBoxLayout*>(ui->headerWidget->layout());
            if (hl) {
                // remove original text label if present to avoid duplicate text
                if (ui->logoLabel) ui->logoLabel->setText("");
                hl->insertWidget(1, titleContainer);
            }
        }
        titleContainer->show();
    }
    // set initial positions for win buttons now that header has size
    if (ui && ui->headerWidget) {
        QPushButton* closeBtn = ui->headerWidget->findChild<QPushButton*>("winCloseBtn");
        QPushButton* miniBtn = ui->headerWidget->findChild<QPushButton*>("winMinBtn");
        int x = ui->headerWidget->width() - 12;
        if (closeBtn) {
            x -= closeBtn->width();
            closeBtn->move(x, 8);
            x -= 8;
        }
        if (miniBtn) {
            x -= miniBtn->width();
            miniBtn->move(x, 8);
            x -= 8;
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
    connect(ui->prevPageButton, &QPushButton::clicked, this, &LiveListWindow::on_prevPageButton_clicked);
    connect(ui->nextPageButton, &QPushButton::clicked, this, &LiveListWindow::on_nextPageButton_clicked);
    
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

    // QML integration removed — keeping original QWidget-based UI
}

LiveListWindow::~LiveListWindow() {
    LOG_INFO("LiveListWindow destroyed");
    delete ui;
}

void LiveListWindow::set_user_info(const QString& user_id, const QString& token) {
    user_id_ = user_id;
    token_ = token;
    
    if (network_manager_) {
        network_manager_->setCredentials(user_id_, token_);
    }
    
    LOG_INFO("User info set: user_id=" + user_id_.toStdString());
}

void LiveListWindow::connect_to_server() {
    if (network_manager_) {
        network_manager_->setServerAddress(server_address_);
        network_manager_->connectToServer();
        LOG_INFO("Connecting to server: " + server_address_.toStdString());
    }
}

void LiveListWindow::setup_live_list() {
    // 清空当前列表
    QLayoutItem* item;
    while ((item = ui->gridLayout->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }
    
    // 设置网格布局的对齐方式为左上对齐，不要居中
    ui->gridLayout->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    
    // Pagination: display subset of live_list_ per page
    const int page_size = 8;
    int total_items = live_list_.size();
    if (total_items == 0) return;
    total_pages_ = (total_items + page_size - 1) / page_size;
    if (current_page_ < 1) current_page_ = 1;
    if (current_page_ > total_pages_) current_page_ = total_pages_;
    int start = (current_page_ - 1) * page_size;
    int end = std::min(start + page_size, total_items);

    for (int idx = start; idx < end; ++idx) {
        int i = idx;
        int pageSlot = idx - start;
        QWidget* card = new QWidget(this);
        card->setFixedSize(280, 250);
        card->setStyleSheet("background-color: rgba(8,6,8,0.6); border: 1px solid rgba(255,255,255,0.06); border-radius: 8px;");

        QVBoxLayout* vbox = new QVBoxLayout(card);
        vbox->setContentsMargins(8, 8, 8, 8);
        vbox->setSpacing(6);

        QLabel* thumb = new QLabel(card);
        thumb->setFixedHeight(140);
        QPixmap pixmap(":/images/Frame back.png");
        if (!pixmap.isNull()) {
            QPixmap scaled = pixmap.scaled(thumb->size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            QPixmap rounded(scaled.size());
            rounded.fill(Qt::transparent);
            {
                QPainter painter(&rounded);
                painter.setRenderHint(QPainter::Antialiasing);
                QPainterPath path;
                const int radius = 8;
                path.addRoundedRect(QRectF(0, 0, scaled.width(), scaled.height()), radius, radius);
                painter.setClipPath(path);
                painter.drawPixmap(0, 0, scaled);
            }
            thumb->setPixmap(rounded);
            thumb->setScaledContents(false);
        } else {
            thumb->setStyleSheet("background-color: rgba(255,255,255,0.06); border-radius:4px;");
        }
        vbox->addWidget(thumb);

        // if live_list_ has data, use it
        QString titleText = QString("测试直播间%1").arg(i+1);
        QString metaText = QString("房间号: %1   %2").arg(100000 + i).arg("2026-01-04 14:00:00");
        if (i < live_list_.size()) {
            QJsonObject obj = live_list_.at(i).toObject();
            titleText = obj.value("name").toString();
            metaText = QString("房间号: %1   %2").arg(obj.value("id").toString()).arg(obj.value("startTime").toString());
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

        // Overlay transparent button to handle clicks
        QPushButton* overlay = new QPushButton(card);
        overlay->setFlat(true);
        overlay->setStyleSheet(
            "QPushButton { background: transparent; border: none; }"
            "QPushButton:hover { border: 1px solid rgba(74,110,240,0.8); }"
        );
        overlay->setGeometry(0, 0, card->width(), card->height());
        overlay->raise();
        connect(overlay, &QPushButton::clicked, [this, i]() { on_live_item_clicked(i); });

        // small icon top-left
        QLabel* icon = new QLabel(card);
        QPixmap iconPix(":/images/Frame_icon.png");
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
        QString status = "未知";
        if (i < live_list_.size()) {
            QJsonObject obj = live_list_.at(i).toObject();
            status = obj.value("status").toString();
        }
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
    
    LOG_INFO("Live list setup completed");
}

void LiveListWindow::load_live_list() {
    // 从网络获取直播列表
    // TODO: 实现与NetworkManager的交互获取直播列表
    // 当前使用模拟数据
    QJsonArray mock_live_list;
    for (int i = 0; i < 20; ++i) {
        QJsonObject live_info;
        live_info["id"] = QString("live_%1").arg(i + 1);
        live_info["name"] = QString("测试直播间%1").arg(i + 1);
        live_info["status"] = "直播中";
        live_info["startTime"] = "2026-01-04 14:00:00";
        mock_live_list.append(live_info);
    }
    
    on_live_list_received(mock_live_list);
    LOG_INFO("Live list loading...");
}

void LiveListWindow::add_live_item(const QJsonObject& live_info) {
    QString live_id = live_info["id"].toString();
    QString live_name = live_info["name"].toString();
    QString live_status = live_info["status"].toString();
    QString start_time = live_info["startTime"].toString();
    
    LOG_INFO("Adding live item: " + live_name.toStdString());
}

void LiveListWindow::on_live_item_clicked(int index) {
    QString live_id = QString("live_%1").arg(index + 1);
    LOG_INFO("Live item clicked: " + live_id.toStdString());
    
    // 发射直播选中信号
    emit live_selected(live_id);
    
    // 关闭当前窗口
    close();
}

void LiveListWindow::on_live_list_received(const QJsonArray& live_list) {
    live_list_ = live_list;
    setup_live_list();
    LOG_INFO("Live list received: " + QString::number(live_list.size()).toStdString() + " items");
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
    // TODO: 实现搜索功能
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
            // reposition control buttons we created (winCloseBtn, winMinBtn) to top-right
            QPushButton* closeBtn = ui->headerWidget->findChild<QPushButton*>("winCloseBtn");
            QPushButton* miniBtn = ui->headerWidget->findChild<QPushButton*>("winMinBtn");
            int x = ui->headerWidget->width() - 12;
            if (closeBtn) {
                x -= closeBtn->width();
                closeBtn->move(x, 8);
                x -= 8;
            }
            if (miniBtn) {
                x -= miniBtn->width();
                miniBtn->move(x, 8);
                x -= 8;
            }
            // ensure initial positions if this is called before any resize
            if (!closeBtn->isVisible()) { /*noop*/ }
            // reposition logo and title container if present
            QLabel* logo = ui->headerWidget->findChild<QLabel*>("logoLabel");
            QWidget* titleContainer = ui->headerWidget->findChild<QWidget*>("titleContainer");
            if (logo) {
                logo->move(12, (ui->headerWidget->height() - logo->height())/2);
            }
            if (titleContainer) {
                int lx = 12 + (logo ? logo->width() : 0) + 8;
                titleContainer->move(lx, (ui->headerWidget->height() - titleContainer->height())/2 - 4);
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

} // namespace live_assistant
