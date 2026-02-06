#ifndef LOGIN_WINDOW_H
#define LOGIN_WINDOW_H

#include <QMainWindow>
#include <QSettings>
#include <QQuickWidget>
#include <QThread>

namespace live_assistant {

class LoginWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit LoginWindow(QWidget *parent = nullptr);
    ~LoginWindow();

signals:
    void login_success();
    void login_failed(const QString& errorMessage);
    void login_status_changed(const QString& status);
    void login_state_changed(bool isLoggingIn, const QString& errorMessage);

public:
    // QML调用方法
    Q_INVOKABLE void qmlLogin(const QString& username, const QString& password);
    Q_INVOKABLE void qmlSetStatus(const QString& status);
    Q_INVOKABLE void qmlSetLoginFailed(const QString& errorMessage);
    Q_INVOKABLE bool qmlHasSavedCredentials();
    Q_INVOKABLE QString qmlGetSavedUsername();
    Q_INVOKABLE QString qmlGetSavedPassword();

    // 获取登录信息
    const QString& user_id() const { return user_id_; }
    const QString& token() const { return token_; }
    const QString& getLoginKey() const { return login_key_; }
    const QString& getLoginUrl() const { return login_url_; }
    const QString& getApiKey() const { return api_key_; }

private slots:
    void onLoginSuccess(const QString& userId, const QString& token, const QString& loginKey);
    void onLoginFailed(const QString& errorMessage);
    void cleanupLoginThread();

private:
    // 保存登录凭据
    void save_login_info_credentials(const QString& username, const QString& password, bool remember);

    // 窗口拖拽支持
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    // 登录信息
    QString user_id_;
    QString token_;
    QString login_key_;
    QString login_url_;
    QString api_key_;

    // QML部件
    QQuickWidget* qmlWidget_;

    // 登录线程
    QThread* loginThread_ = nullptr;

    // 设置
    QSettings settings_;

    // 窗口拖拽
    bool dragging_ = false;
    QPoint dragStartPos_;
};

} // namespace live_assistant

#endif // LOGIN_WINDOW_H
