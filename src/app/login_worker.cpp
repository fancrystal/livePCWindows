#include "app/login_worker.h"
#include "app/encryption_utils.h"
#include "app/login_service.h"
#include "common/log.h"

LoginWorker::LoginWorker(const QString& loginUrl, const QString& apiKey,
                         const QString& username, const QString& password,
                         bool remember,
                         QObject* parent)
    : QObject(parent)
    , loginUrl_(loginUrl)
    , apiKey_(apiKey)
    , username_(username)
    , password_(password)
    , remember_(remember)
    , settings_("LiveAssistant", "Login")
{
}

LoginWorker::~LoginWorker()
{
}

void LoginWorker::startLogin()
{
    QString errMessage;
    QString userId;
    QString token;

    LOG_INFO(QString("后台线程开始登录: username length=%1").arg(username_.length()).toStdString());

    // 调用登录API
    LoginService* loginService = LoginService::instance();
    bool success = loginService->login(loginUrl_, apiKey_, username_, password_, userId, token, errMessage);

    if (success) {
        // 获取一次性登录密钥
        QString loginKey;
        QString keyError;
        if (loginService->genOnceLoginKey(loginUrl_, userId, token, loginKey, keyError)) {
            LOG_INFO("后台线程获取登录密钥成功");
        } else {
            LOG_WARNING(QString("后台线程获取登录密钥失败: %1").arg(keyError).toStdString());
        }

        // 保存登录凭据
        settings_.setValue("username", username_);
        if (remember_) {
            QString protectedPassword = EncryptionUtils::protectForCurrentUser(password_);
            if (protectedPassword.isEmpty()) {
                LOG_WARNING("Failed to protect saved login password; password will not be persisted");
                settings_.remove("password");
                settings_.remove("passwordProtected");
            } else {
                settings_.setValue("passwordProtected", protectedPassword);
                settings_.remove("password");
            }
        } else {
            settings_.remove("password");
            settings_.remove("passwordProtected");
        }
        settings_.setValue("remember", remember_);
        LOG_INFO(QString("后台线程保存登录凭据成功: remember=%1").arg(remember_).toStdString());

        LOG_INFO(QString("后台线程登录成功: userId=%1").arg(userId).toStdString());
        emit loginSuccess(userId, token, loginKey);
    } else {
        QString errorMsg = errMessage.isEmpty() ? "用户名或密码错误" : errMessage;
        LOG_WARNING(QString("后台线程登录失败: %1").arg(errorMsg).toStdString());
        emit loginFailed(errorMsg);
    }
}
