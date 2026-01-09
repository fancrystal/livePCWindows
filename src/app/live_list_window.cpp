#include "app/live_list_window.h"
#include "ui_live_list_window.h"
#include "common/log.h"
#include <QJsonDocument>
#include <QMessageBox>
#include <QPushButton>
#include <QGridLayout>

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
    
    // 设置样式表
    setStyleSheet(
        "QMainWindow { background-color: #f5f5f5; }");
    
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
    
    // 初始化直播列表
    setup_live_list();
    
    LOG_INFO("LiveListWindow created");
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
    
    // 模拟数据：创建测试直播间
    int live_count = 1; // 只有一个直播间
    
    // 2*4的网格布局，最多8个直播间
    for (int i = 0; i < live_count; ++i) {
        QPushButton* live_item = new QPushButton(this);
        live_item->setFixedSize(280, 250); // 固定大小，不要拉升
        live_item->setStyleSheet(
            "QPushButton { border: 1px solid #eee; border-radius: 8px; background-color: white; text-align: center; }"
            "QPushButton:hover { border-color: #4CAF50; }"
        );
        
        QString live_name = QString("测试直播间");
        QString live_time = QString("2026-01-04 14:00:00");
        
        QString live_content = QString("%1\n%2\n状态: 直播中").arg(live_name).arg(live_time);
        live_item->setText(live_content);
        
        // 连接点击事件
        connect(live_item, &QPushButton::clicked, [this, i]() {
            on_live_item_clicked(i);
        });
        
        // 添加到网格布局，2*4的样式，从第一行第一列开始排列
        int row = i / 4;
        int col = i % 4;
        ui->gridLayout->addWidget(live_item, row, col);
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
    for (int i = 0; i < 8; ++i) {
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
        load_live_list();
        LOG_INFO("Prev page: " + QString::number(current_page_).toStdString());
    }
}

void LiveListWindow::on_nextPageButton_clicked() {
    if (current_page_ < total_pages_) {
        current_page_++;
        load_live_list();
        LOG_INFO("Next page: " + QString::number(current_page_).toStdString());
    }
}

void LiveListWindow::update_pagination() {
    // 只有一个直播间，不需要分页
    bool show_pagination = false;
    
    // 隐藏所有分页按钮
    ui->prevPageButton->setVisible(show_pagination);
    ui->page1Button->setVisible(show_pagination);
    ui->page2Button->setVisible(show_pagination);
    ui->page3Button->setVisible(show_pagination);
    ui->page4Button->setVisible(show_pagination);
    ui->page5Button->setVisible(show_pagination);
    ui->ellipsisLabel->setVisible(show_pagination);
    ui->lastPageButton->setVisible(show_pagination);
    ui->nextPageButton->setVisible(show_pagination);
}

void LiveListWindow::on_page_button_clicked() {
    QPushButton* sender_button = qobject_cast<QPushButton*>(sender());
    if (sender_button) {
        int page = sender_button->text().toInt();
        if (page != current_page_ && page > 0 && page <= total_pages_) {
            current_page_ = page;
            load_live_list();
            LOG_INFO("Page button clicked: " + QString::number(current_page_).toStdString());
            
            // 更新页按钮样式
            QList<QPushButton*> page_buttons = {
                ui->page1Button, ui->page2Button, ui->page3Button, ui->page4Button, ui->page5Button
            };
            
            for (QPushButton* button : page_buttons) {
                int button_page = button->text().toInt();
                if (button_page == current_page_) {
                    button->setStyleSheet("background-color: #4CAF50; color: white; border-radius: 4px;");
                } else {
                    button->setStyleSheet("");
                }
            }
        }
    }
}

} // namespace live_assistant