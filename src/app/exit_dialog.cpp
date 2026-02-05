#include "app/exit_dialog.h"
#include "ui_exit_dialog.h"
#include "common/log.h"

#include <QShowEvent>
#include <QHideEvent>
#include <QGuiApplication>
#include <QScreen>

namespace live_assistant {

ExitDialog::ExitDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::ExitDialog) {
    ui->setupUi(this);
    
    // 设置窗口属性 - 无边框对话框
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setModal(true);
    
    // 设置固定大小
    setFixedSize(320, 220);
    
    // 初始化选择状态（读取默认选中的单选按钮）
    if (ui->radioButton_exit->isChecked()) {
        selected_action_ = Action::Exit;
    } else if (ui->radioButton_minimize->isChecked()) {
        selected_action_ = Action::Minimize;
    }
    
    // 应用样式
    applyStyle();
    
    // 连接信号槽
    setupConnections();
    
    LOG_INFO("ExitDialog created");
}

ExitDialog::~ExitDialog() {
    delete ui;
    LOG_INFO("ExitDialog destroyed");
}

void ExitDialog::setupConnections() {
    // 单选按钮状态变化
    connect(ui->radioButton_exit, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked) {
            selected_action_ = Action::Exit;
        }
    });
    
    connect(ui->radioButton_minimize, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked) {
            selected_action_ = Action::Minimize;
        }
    });
    
    // 记住选择复选框
    if (ui->checkBox_remember) {
        connect(ui->checkBox_remember, &QCheckBox::toggled,
                this, [this](bool checked) {
            remember_choice_ = checked;
        });
    }
    
    // 取消按钮
    if (ui->pushButton_cancel) {
        connect(ui->pushButton_cancel, &QPushButton::clicked,
                this, &ExitDialog::reject);
    }
    
    // 确认按钮
    if (ui->pushButton_confirm) {
        connect(ui->pushButton_confirm, &QPushButton::clicked,
                this, &ExitDialog::accept);
    }
}

void ExitDialog::applyStyle() {
    setStyleSheet(R"(
        QDialog {
            background-color: #282C34;
            border-radius: 8px;
        }
        QWidget {
            background-color: #282C34;
            color: #ffffff;
        }
        QLabel {
            background-color: transparent;
            color: #ffffff;
        }
        QRadioButton {
            background-color: transparent;
            color: #ffffff;
            font-size: 13px;
            spacing: 6px;
        }
        QRadioButton::indicator {
            width: 14px;
            height: 14px;
            border: 2px solid #555555;
            border-radius: 7px;
            background-color: transparent;
        }
        QRadioButton::indicator:hover {
            border-color: #4a6ef0;
        }
        QRadioButton::indicator:checked {
            background-color: #4a6ef0;
            border-color: #4a6ef0;
        }
        QCheckBox {
            background-color: transparent;
            color: #888888;
            font-size: 12px;
        }
        QCheckBox::indicator {
            width: 14px;
            height: 14px;
            border: 1px solid #444444;
            border-radius: 3px;
            background-color: #2a2a2a;
        }
        QCheckBox::indicator:hover {
            border-color: #4a6ef0;
        }
        QCheckBox::indicator:checked {
            background-color: #4a6ef0;
            border-color: #4a6ef0;
        }
        QPushButton {
            border-radius: 4px;
            font-size: 12px;
        }
        QPushButton#pushButton_cancel {
            background-color: #3a3f4a;
            color: #ffffff;
            min-width: 50px;
            min-height: 22px;
            border: none;
        }
        QPushButton#pushButton_cancel:hover {
            background-color: #4a5060;
        }
        QPushButton#pushButton_confirm {
            background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #3a5fd0, stop:1 #2a4ec0);
            color: #ffffff;
            min-width: 50px;
            min-height: 22px;
            border: none;
        }
        QPushButton#pushButton_confirm:hover {
            background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #4a6ff0, stop:1 #3a5fe0);
        }
    )");
}

void ExitDialog::setWindowIcon(const QIcon &icon) {
    QDialog::setWindowIcon(icon);
}

void ExitDialog::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    
    // 居中显示
    QWidget *parent = parentWidget();
    if (parent) {
        QPoint parentCenter = parent->geometry().center();
        move(parentCenter.x() - width() / 2, parentCenter.y() - height() / 2);
    } else {
        if (QScreen *screen = QGuiApplication::primaryScreen()) {
            QRect screenGeometry = screen->availableGeometry();
            move(screenGeometry.center().x() - width() / 2, screenGeometry.center().y() - height() / 2);
        }
    }
}

} // namespace live_assistant
