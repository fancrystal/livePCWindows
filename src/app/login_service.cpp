#include "app/login_service.h"
#include "http/http_client.h"
#include "app/encryption_utils.h"
#include "common/log.h"
#include <QJsonObject>
#include <QJsonDocument>

namespace {
QString makeAuthorizationHeader(const QString& token)
{
    QString normalized = token.trimmed();
    if (normalized.startsWith("Bearer ", Qt::CaseInsensitive)) {
        normalized = normalized.mid(7).trimmed();
    }
    return QString("Bearer %1").arg(normalized);
}
}

LoginService* LoginService::m_instance = nullptr;
QMutex LoginService::m_mutex;

LoginService* LoginService::instance()
{
    QMutexLocker locker(&m_mutex);
    if (!m_instance) {
        m_instance = new LoginService();
    }
    return m_instance;
}

bool LoginService::login(const QString &loginUrl, const QString &key, const QString &username,
                         const QString &password, QString &userID, QString &token, QString &errMessage)
{
    errMessage.clear();
    userID.clear();
    token.clear();

    // 分别对用户名和密码进行AES128_ECB加密
    QString encryptedUserTel = EncryptionUtils::encryptAES128_ECB(username, key);
    QString encryptedUserPwd = EncryptionUtils::encryptAES128_ECB(password, key);

    // 检查加密结果
    if (loginUrl.isEmpty() || encryptedUserTel.isEmpty() || encryptedUserPwd.isEmpty()) {
        errMessage = "Failed to encrypt login data";
        LOG_WARNING(errMessage.toStdString());
        return false;
    }

    // 构造POST请求的JSON数据
    QJsonObject postData;
    postData["userTel"] = encryptedUserTel;
    postData["userPwd"] = encryptedUserPwd;
    QString url = QString("%1/auth/ClientPwdLogin").arg(loginUrl);

    // 发送POST请求
    HttpClient* client = HttpClient::instance();
    LOG_INFO(QString("Sending login request to: %1").arg(url).toStdString());
    QString httpErrMsg;
    QJsonObject response = client->post(url, postData, httpErrMsg);

    // 如果HTTP请求失败，返回HTTP错误信息
    if (!httpErrMsg.isEmpty()) {
        errMessage = httpErrMsg;
        LOG_WARNING(QString("Login HTTP request failed: %1").arg(errMessage).toStdString());
        return false;
    }

    LOG_INFO(QString("Login response: %1").arg(QJsonDocument(response).toJson(QJsonDocument::Compact).constData()).toStdString());

    // 处理响应结果
    if (response["code"].toInt() == 200) {
        token = response["data"].toObject()["token"].toString();
        userID = response["data"].toObject()["userId"].toString();
        userId_ = userID;
        token_ = token;
        LOG_INFO(QString("Login succeed, userID: %1").arg(userID).toStdString());
        return true;
    }

    errMessage = response["msg"].toString();
    LOG_WARNING(QString("Login failed: %1").arg(errMessage).toStdString());
    return false;
}

bool LoginService::genOnceLoginKey(const QString &baseUrl, const QString &userId,
                                   const QString &token, QString &loginKey, QString &errMessage)
{
    errMessage.clear();
    loginKey.clear();

    // 构造API请求URL
    QString url = QString("%1/accounts/GenUserOnceLoginKey?userId=%2").arg(baseUrl).arg(userId);

    // 发送GET请求
    HttpClient* client = HttpClient::instance();

    struct curl_slist* headers = client->createHeaders();
    client->addHeader(&headers, "Authorization", makeAuthorizationHeader(token));
    client->addHeader(&headers, "Content-Type", "application/json");

    QString httpErrMsg;
    QJsonObject response = client->get(url, headers, httpErrMsg);
    client->freeHeaders(headers);

    // 如果HTTP请求失败，返回HTTP错误信息
    if (!httpErrMsg.isEmpty()) {
        errMessage = httpErrMsg;
        LOG_WARNING(QString("Generate login key HTTP request failed: %1").arg(errMessage).toStdString());
        return false;
    }

    // 处理响应结果
    if (response["code"].toInt() == 200) {
        loginKey = response["data"].toString();
        loginKey_ = loginKey;
        LOG_INFO(QString("Generate once login key succeed").toStdString());
        return true;
    }

    errMessage = response["msg"].toString();
    LOG_WARNING(QString("Generate once login key failed: %1").arg(errMessage).toStdString());
    return false;
}

void LoginService::logout()
{
    userId_.clear();
    token_.clear();
    loginKey_.clear();
    LOG_INFO("Logged out");
}
