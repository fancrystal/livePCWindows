#ifndef LOGIN_WINDOW_H
#define LOGIN_WINDOW_H

#include <QMainWindow>
#include <QSettings>
#include <QQuickWidget>

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

    Q_INVOKABLE void qmlLogin(const QString& username, const QString& password);

private:
    void load_login_info();
    void save_login_info();
    void save_login_info_credentials(const QString& username, const QString& password, bool remember);
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
    QQuickWidget* qmlWidget_;
    // frameless window drag support
    bool dragging_;
    QPoint dragStartPos_;

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
};

} // namespace live_assistant

#endif // LOGIN_WINDOW_H
