#ifndef LOGIN_SERVICE_H
#define LOGIN_SERVICE_H

#include <QString>
#include <QMutex>

class LoginService
{
public:
    static LoginService* instance();

    // 登录
    bool login(const QString &loginUrl, const QString &key, const QString &username, const QString &password,
               QString &userID, QString &token, QString &errMessage);

    // 获取一次性登录密钥
    bool genOnceLoginKey(const QString &baseUrl, const QString &userId, const QString &token,
                         QString &loginKey, QString &errMessage);

    // 获取用户ID
    QString getUserId() const { return userId_; }

    // 获取Token
    QString getToken() const { return token_; }

    // 获取登录密钥
    QString getLoginKey() const { return loginKey_; }

    // 是否已登录
    bool isLoggedIn() const { return !token_.isEmpty(); }

    // 登出
    void logout();

private:
    LoginService() = default;
    ~LoginService() = default;

    LoginService(const LoginService&) = delete;
    LoginService& operator=(const LoginService&) = delete;

    static LoginService* m_instance;
    static QMutex m_mutex;

    QString userId_;
    QString token_;
    QString loginKey_;
    QString loginUrl_;
};

#endif // LOGIN_SERVICE_H
