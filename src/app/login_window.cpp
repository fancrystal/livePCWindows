#include "app/login_window.h"
#include "ui_login_window.h"
#include "common/log.h"

#include <QMessageBox>
#include <QRegularExpression>
#include <QSettings>

namespace live_assistant {

LoginWindow::LoginWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::LoginWindow),
    is_password_login_(true),
    settings_("LiveAssistant", "Login") {
    ui->setupUi(this);
    
    // 设置窗口标题
    setWindowTitle("启点点直播 - 登录");
    
    // 初始化登录URL和API密钥
    login_url_ = "http://api.example.com";
    api_key_ = "your_api_key";
    
    // 默认勾选已阅读并同意和记住密码
    ui->agreementCheckBox->setChecked(true);
    ui->rememberPasswordCheckBox->setChecked(true);
    
    // 加载保存的登录信息
    load_login_info();
    
    // 初始化为密码登录模式
    on_passwordLoginButton_clicked();
    
    LOG_INFO("LoginWindow created");
}

LoginWindow::~LoginWindow() {
    LOG_INFO("LoginWindow destroyed");
    delete ui;
}

void LoginWindow::on_passwordLoginButton_clicked() {
    // 切换到密码登录模式
    is_password_login_ = true;
    ui->passwordLoginButton->setStyleSheet(
        "QPushButton {\n"
        "    background-color: transparent;\n"
        "    color: #1a1a1a;\n"
        "    border: none;\n"
        "    border-bottom: 2px solid #4CAF50;\n"
        "    padding: 8px 16px;\n"
        "    font-size: 14px;\n"
        "    font-weight: bold;\n"
        "}\n"
        "QPushButton:hover {\n"
        "    color: #4CAF50;\n"
        "}");
    
    ui->verificationCodeLoginButton->setStyleSheet(
        "QPushButton {\n"
        "    background-color: transparent;\n"
        "    color: #666666;\n"
        "    border: none;\n"
        "    border-bottom: 2px solid transparent;\n"
        "    padding: 8px 16px;\n"
        "    font-size: 14px;\n"
        "    font-weight: bold;\n"
        "}\n"
        "QPushButton:hover {\n"
        "    color: #4CAF50;\n"
        "    border-bottom: 2px solid #4CAF50;\n"
        "}");
    
    // 显示密码输入框，隐藏验证码输入框
    ui->passwordLineEdit->show();
    ui->verificationCodeLineEdit->hide();
    ui->getVerificationCodeButton->hide();
    
    // 保存当前用户名，避免切换模式时丢失
    QString current_username = ui->accountLineEdit->text().trimmed();
    
    ui->accountLineEdit->setPlaceholderText("请输入账号名/账号ID");
    ui->passwordLineEdit->setPlaceholderText("请输入登录密码");
    
    // 恢复用户名和密码（如果有保存）
    if (!current_username.isEmpty()) {
        ui->accountLineEdit->setText(current_username);
    }
    
    // 重新加载保存的密码
    QString password = settings_.value("password").toString();
    bool remember = settings_.value("remember", false).toBool();
    if (remember) {
        ui->passwordLineEdit->setText(password);
        ui->rememberPasswordCheckBox->setChecked(true);
    }
    
    LOG_INFO("Switched to password login mode");
}

void LoginWindow::on_verificationCodeLoginButton_clicked() {
    // 切换到验证码登录模式
    is_password_login_ = false;
    ui->verificationCodeLoginButton->setStyleSheet(
        "QPushButton {\n"
        "    background-color: transparent;\n"
        "    color: #1a1a1a;\n"
        "    border: none;\n"
        "    border-bottom: 2px solid #4CAF50;\n"
        "    padding: 8px 16px;\n"
        "    font-size: 14px;\n"
        "    font-weight: bold;\n"
        "}\n"
        "QPushButton:hover {\n"
        "    color: #4CAF50;\n"
        "}");
    
    ui->passwordLoginButton->setStyleSheet(
        "QPushButton {\n"
        "    background-color: transparent;\n"
        "    color: #666666;\n"
        "    border: none;\n"
        "    border-bottom: 2px solid transparent;\n"
        "    padding: 8px 16px;\n"
        "    font-size: 14px;\n"
        "    font-weight: bold;\n"
        "}\n"
        "QPushButton:hover {\n"
        "    color: #4CAF50;\n"
        "    border-bottom: 2px solid #4CAF50;\n"
        "}");
    
    // 隐藏密码输入框，显示验证码输入框
    ui->passwordLineEdit->hide();
    ui->verificationCodeLineEdit->show();
    ui->getVerificationCodeButton->show();
    
    // 保存当前用户名，避免切换模式时丢失
    QString current_username = ui->accountLineEdit->text().trimmed();
    
    ui->accountLineEdit->setPlaceholderText("请输入手机号");
    
    // 恢复用户名（如果有）
    if (!current_username.isEmpty()) {
        ui->accountLineEdit->setText(current_username);
    }
    
    LOG_INFO("Switched to verification code login mode");
}

void LoginWindow::load_login_info() {
    // 从QSettings中加载登录信息
    QString username = settings_.value("username").toString();
    QString password = settings_.value("password").toString();
    bool remember = settings_.value("remember", false).toBool();
    
    // 设置UI控件
    ui->accountLineEdit->setText(username);
    if (remember) {
        ui->passwordLineEdit->setText(password);
        ui->rememberPasswordCheckBox->setChecked(true);
    }
    
    LOG_INFO("Loaded login info: username=" + username.toStdString() + ", remember=" + (remember ? "true" : "false"));
}

void LoginWindow::save_login_info() {
    // 保存登录信息到QSettings
    QString username = ui->accountLineEdit->text().trimmed();
    QString password = ui->passwordLineEdit->text();
    bool remember = ui->rememberPasswordCheckBox->isChecked();
    
    settings_.setValue("username", username);
    if (remember) {
        settings_.setValue("password", password);
    } else {
        settings_.remove("password");
    }
    settings_.setValue("remember", remember);
    
    LOG_INFO("Saved login info: username=" + username.toStdString() + ", remember=" + (remember ? "true" : "false"));
}

bool LoginWindow::validate_login(const QString& username, const QString& password) {
    // 暂时使用模拟登录逻辑，因为HTTP模块还未完全集成
    LOG_INFO("模拟登录验证: username=" + username.toStdString() + ", password=" + password.toStdString());
    
    // 模拟登录成功
    user_id_ = "test_user_id";
    token_ = "test_token";
    login_key_ = "test_login_key";
    
    return true;
}

void LoginWindow::on_loginButton_clicked() {
    // 验证输入
    QString username = ui->accountLineEdit->text().trimmed();
    QString password = ui->passwordLineEdit->text();
    
    if (username.isEmpty() || password.isEmpty()) {
        ui->statusLabel->setText("用户名和密码不能为空");
        return;
    }
    
    if (!ui->agreementCheckBox->isChecked()) {
        ui->statusLabel->setText("请阅读并同意服务条款和隐私协议");
        return;
    }
    
    // 验证登录信息
    if (validate_login(username, password)) {
        // 保存登录信息
        save_login_info();
        
        // 登录成功
        LOG_INFO("Login successful: " + username.toStdString());
        
        // 发射登录成功信号
        emit login_success();
        
        // 关闭登录窗口
        close();
    } else {
        ui->statusLabel->setText("用户名或密码错误");
        ui->passwordLineEdit->clear();
    }
}

void LoginWindow::on_getVerificationCodeButton_clicked() {
    QString account = ui->accountLineEdit->text().trimmed();
    
    // 验证码登录模式下，只需要验证手机号
    if (account.isEmpty()) {
        QMessageBox::warning(this, "提示", "请输入手机号");
        return;
    }
    
    // 验证手机号格式
    QRegularExpression phone_regex("^1[3-9]\\d{9}$");
    if (!phone_regex.match(account).hasMatch()) {
        QMessageBox::warning(this, "提示", "请输入有效的手机号");
        return;
    }
    
    // 模拟发送验证码
    LOG_INFO("Verification code sent to: " + account.toStdString());
    QMessageBox::information(this, "提示", "验证码已发送，请注意查收");
}

void LoginWindow::on_agreementLabel_linkActivated(const QString &link) {
    // 处理服务条款和隐私协议的点击事件
    QMessageBox::information(this, "提示", "服务条款和隐私协议");
    LOG_INFO("Agreement link clicked");
}

} // namespace live_assistant
