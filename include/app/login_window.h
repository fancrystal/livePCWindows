#ifndef LOGIN_WINDOW_H
#define LOGIN_WINDOW_H

#include <QMainWindow>
#include <QSettings>

namespace Ui {
class LoginWindow;
}

namespace live_assistant {

class LoginWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit LoginWindow(QWidget *parent = nullptr);
    ~LoginWindow();

signals:
    void login_success();

private slots:
    void on_passwordLoginButton_clicked();
    void on_verificationCodeLoginButton_clicked();
    void on_loginButton_clicked();
    void on_getVerificationCodeButton_clicked();
    void on_agreementLabel_linkActivated(const QString &link);

public:
    const QString& user_id() const { return user_id_; }
    const QString& token() const { return token_; }

private:
    void load_login_info();
    void save_login_info();
    bool validate_login(const QString& username, const QString& password);

private:
    Ui::LoginWindow *ui;
    bool is_password_login_;
    QString user_id_;
    QString token_;
    QString login_key_;
    QString login_url_;
    QString api_key_;
    QSettings settings_;
};

} // namespace live_assistant

#endif // LOGIN_WINDOW_H
