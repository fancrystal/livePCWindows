#include "app/share_settings_dialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QMessageBox>
#include <QGuiApplication>
#include <QScreen>
#include <QCloseEvent>
#include <QSysInfo>

#ifdef _WIN32
#include <windows.h>
#endif

namespace live_assistant {

ShareSettingsDialog::ShareSettingsDialog(QWidget* parent)
    : QDialog(parent) {
    setupUI();
    applyStyle();

    // 检查 Win11 支持（只在 Win10 时禁用控件并显示警告）
    if (!checkWin11Support()) {
        if (check_cursor_) check_cursor_->setEnabled(false);
        if (check_border_) check_border_->setEnabled(false);
    }
}

void ShareSettingsDialog::setInitialValues(bool capture_cursor, bool capture_border) {
    // 更新成员变量
    capture_cursor_ = capture_cursor;
    capture_border_ = capture_border;

    // 立即更新已创建的 checkbox（如果 setupUI 已经执行）
    if (check_cursor_) {
        check_cursor_->setChecked(capture_cursor);
        check_cursor_->setEnabled(true);
    }
    if (check_border_) {
        check_border_->setChecked(capture_border);
        check_border_->setEnabled(true);
    }
}

ShareSettingsDialog::~ShareSettingsDialog() {
}

void ShareSettingsDialog::setupUI() {
    // 设置窗口属性
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setModal(true);
    setFixedSize(360, 240);

    // 主布局
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(20, 20, 20, 15);
    mainLayout->setSpacing(15);

    // 标题
    auto* titleLayout = new QHBoxLayout();
    QLabel* titleLabel = new QLabel(QString::fromUtf8("共享设置"), this);
    titleLabel->setStyleSheet("font-size: 16px; font-weight: bold; color: #ffffff;");
    titleLayout->addWidget(titleLabel);
    titleLayout->addStretch();
    mainLayout->addLayout(titleLayout);

    // 分隔线
    QFrame* line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setStyleSheet("background-color: #3a3a3a;");
    mainLayout->addWidget(line);

    // Win11 警告标签（仅 Win10 显示）
    warning_label_ = new QLabel(this);
    warning_label_->setWordWrap(true);
    warning_label_->setStyleSheet(
        "color: #ffaa00; font-size: 12px; background-color: rgba(255, 170, 0, 0.1); "
        "padding: 8px; border-radius: 4px; border: 1px solid #ffaa00;"
    );
    warning_label_->setVisible(false);
    mainLayout->addWidget(warning_label_);

    // 选项布局
    auto* optionLayout = new QVBoxLayout();
    optionLayout->setSpacing(12);

    // 捕获鼠标选项
    check_cursor_ = new QCheckBox(QString::fromUtf8("捕获鼠标指针"), this);
    check_cursor_->setChecked(capture_cursor_);
    optionLayout->addWidget(check_cursor_);

    // 显示捕获黄框选项
    check_border_ = new QCheckBox(QString::fromUtf8("显示窗口选择边框"), this);
    check_border_->setChecked(capture_border_);
    check_border_->setToolTip(QString::fromUtf8("在捕获窗口周围显示黄色边框（仅窗口模式）"));
    optionLayout->addWidget(check_border_);

    mainLayout->addLayout(optionLayout);

    mainLayout->addStretch();

    // 按钮布局
    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();

    QPushButton* cancelBtn = new QPushButton(QString::fromUtf8("取消"), this);
    cancelBtn->setObjectName("cancelBtn");
    connect(cancelBtn, &QPushButton::clicked, this, &ShareSettingsDialog::reject);
    btnLayout->addWidget(cancelBtn);

    QPushButton* confirmBtn = new QPushButton(QString::fromUtf8("确定"), this);
    confirmBtn->setObjectName("confirmBtn");
    connect(confirmBtn, &QPushButton::clicked, this, [this]() {
        capture_cursor_ = check_cursor_->isChecked();
        capture_border_ = check_border_->isChecked();
        accept();
    });
    btnLayout->addWidget(confirmBtn);

    mainLayout->addLayout(btnLayout);
}

void ShareSettingsDialog::applyStyle() {
    setStyleSheet(R"(
        QDialog {
            background-color: #282C34;
            border-radius: 8px;
        }
        QCheckBox {
            color: #ffffff;
            font-size: 14px;
            spacing: 8px;
            padding: 4px;
        }
        QCheckBox:hover {
            background-color: rgba(255, 255, 255, 0.05);
        }
        QCheckBox::indicator {
            width: 18px;
            height: 18px;
            border: 2px solid #666666;
            border-radius: 4px;
            background-color: #333333;
        }
        QCheckBox::indicator:hover {
            border-color: #4a6ef0;
            background-color: #3a3a3a;
        }
        QCheckBox::indicator:checked {
            background-color: #4a6ef0;
            border-color: #4a6ef0;
        }
        QCheckBox::indicator:checked:hover {
            background-color: #5a7eff;
            border-color: #5a7eff;
        }
        QPushButton {
            border-radius: 4px;
            font-size: 13px;
            min-width: 70px;
            min-height: 30px;
        }
        QPushButton#cancelBtn {
            background-color: #3a3f4a;
            color: #ffffff;
            border: none;
        }
        QPushButton#cancelBtn:hover {
            background-color: #4a5060;
        }
        QPushButton#confirmBtn {
            background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #4a6ef0, stop:1 #3a5fd0);
            color: #ffffff;
            border: none;
        }
        QPushButton#confirmBtn:hover {
            background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #5a7eff, stop:1 #4a6fe0);
        }
    )");
}

bool ShareSettingsDialog::checkWin11Support() {
    // 使用 Windows API 检查版本
    // Windows 11 版本号 >= 10.0.22000
    bool isWin11 = false;

#ifdef _WIN32
    OSVERSIONINFOEXW osvi = {0};
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEXW);
    osvi.dwMajorVersion = 10;
    osvi.dwBuildNumber = 22000;

    // 使用 VerifyVersionInfoW 检查是否 >= Windows 10.0.22000
    ULONGLONG conditionMask = 0;
    conditionMask = VerSetConditionMask(conditionMask, VER_MAJORVERSION, VER_GREATER_EQUAL);
    conditionMask = VerSetConditionMask(conditionMask, VER_BUILDNUMBER, VER_GREATER_EQUAL);

    isWin11 = VerifyVersionInfoW(&osvi, VER_MAJORVERSION | VER_BUILDNUMBER, conditionMask);
#endif

    if (!isWin11) {
        warning_label_->setVisible(true);
        warning_label_->setText(
            QString::fromUtf8("⚠ Windows 10 系统不支持 WGC 共享功能。\n"
                             "请升级到 Windows 11 以使用屏幕/窗口共享。")
        );
    }

    return isWin11;
}

void ShareSettingsDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);

    // 居中显示
    QWidget* parent = parentWidget();
    if (parent) {
        QPoint parentCenter = parent->geometry().center();
        move(parentCenter.x() - width() / 2, parentCenter.y() - height() / 2);
    } else {
        if (QScreen* screen = QGuiApplication::primaryScreen()) {
            QRect screenGeometry = screen->availableGeometry();
            move(screenGeometry.center().x() - width() / 2, screenGeometry.center().y() - height() / 2);
        }
    }
}

void ShareSettingsDialog::closeEvent(QCloseEvent* event) {
    // 保存设置到成员变量
    capture_cursor_ = check_cursor_->isChecked();
    capture_border_ = check_border_->isChecked();
    event->accept();
}

} // namespace live_assistant
