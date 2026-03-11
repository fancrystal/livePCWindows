#include "http/http_client.h"
#include "common/log.h"
#include <QJsonDocument>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>

HttpClient::HttpClient(QObject* parent) : QObject(parent)
{
    // 不再在构造函数中初始化CURL库，改为在main函数中调用globalInit()
}

HttpClient::~HttpClient()
{
    // 不再在析构函数中清理CURL库，改为在main函数中调用globalCleanup()
}

// 静态成员变量定义
HttpClient* HttpClient::m_instance = nullptr;
QMutex HttpClient::m_mutex;
bool HttpClient::m_curlGlobalInitialized = false;
QMutex HttpClient::m_globalMutex;

HttpClient* HttpClient::instance()
{
    QMutexLocker locker(&m_mutex);
    if(!m_instance) {
        m_instance = new HttpClient();
    }
    return m_instance;
}

// 全局初始化CURL库，应该只在主线程中调用一次
void HttpClient::globalInit()
{
    QMutexLocker locker(&m_globalMutex);
    if(!m_curlGlobalInitialized) {
        // 使用CURL_GLOBAL_NOTHING选项，只初始化基本的CURL功能，不初始化SSL、zlib等可能不是线程安全的子系统
        curl_global_init(CURL_GLOBAL_NOTHING);
        m_curlGlobalInitialized = true;
    }
}

// 全局清理CURL库，应该只在程序退出时调用一次
void HttpClient::globalCleanup()
{
    QMutexLocker locker(&m_globalMutex);
    if(m_curlGlobalInitialized) {
        curl_global_cleanup();
        m_curlGlobalInitialized = false;
    }
}

size_t HttpClient::writeCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    QByteArray* buffer = static_cast<QByteArray*>(userp);
    size_t realsize = size * nmemb;
    buffer->append(static_cast<char*>(contents), realsize);
    return realsize;
}

// 创建请求头列表
struct curl_slist* HttpClient::createHeaders()
{
    return nullptr;
}

// 添加请求头
void HttpClient::addHeader(struct curl_slist** headers, const QString& key, const QString& value)
{
    QString header = QString("%1: %2").arg(key).arg(value);
    *headers = curl_slist_append(*headers, header.toUtf8().constData());
}

// 释放请求头
void HttpClient::freeHeaders(struct curl_slist* headers)
{
    if(headers) {
        curl_slist_free_all(headers);
    }
}

QJsonObject HttpClient::executeRequest(CURL* curl, const QString& url, struct curl_slist* headers, QString& errMsg)
{
    QByteArray responseBuffer;

    curl_easy_setopt(curl, CURLOPT_URL, url.toUtf8().constData());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);

    // 🔧 添加超时设置（3秒）
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);  // 整体超时3秒
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);  // 连接超时3秒

    QString caCertPath = QCoreApplication::applicationDirPath() + "/resources/cacert.pem";
    curl_easy_setopt(curl, CURLOPT_CAINFO, caCertPath.toUtf8().constData());
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L); // 启用验证（默认）
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L); // 验证主机名

    if(headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    CURLcode res = curl_easy_perform(curl);

    if(res != CURLE_OK) {
        QString errorMsg;
        switch(res) {
            case CURLE_OPERATION_TIMEDOUT:
                errorMsg = "网络请求超时，请检查网络连接";
                break;
            case CURLE_COULDNT_CONNECT:
                errorMsg = "无法连接到服务器，请检查网络";
                break;
            case CURLE_COULDNT_RESOLVE_HOST:
                errorMsg = "无法解析服务器地址";
                break;
            case CURLE_SSL_CONNECT_ERROR:
                errorMsg = "SSL连接失败";
                break;
            case CURLE_SSL_CERTPROBLEM:
                errorMsg = "SSL证书问题";
                break;
            case CURLE_SSL_CACERT:
                errorMsg = "SSL CA证书验证失败";
                break;
            default:
                errorMsg = QString("网络错误: %1").arg(curl_easy_strerror(res));
                break;
        }
        errMsg = errorMsg;
        LOG_WARNING(QString("HTTP request failed: %1").arg(errorMsg).toStdString());
        return QJsonObject();
    }

    QJsonDocument doc = QJsonDocument::fromJson(responseBuffer);
    return doc.object();
}

QJsonObject HttpClient::post(const QString& url, const QJsonObject& data, QString& errMsg)
{
    // 使用默认的Content-Type头
    struct curl_slist* headers = createHeaders();
    addHeader(&headers, "Content-Type", "application/json");
    QJsonObject result = post(url, data, headers, errMsg);
    freeHeaders(headers);
    return result;
}

// 带自定义headers的POST请求
QJsonObject HttpClient::post(const QString& url, const QJsonObject& data, struct curl_slist* headers, QString& errMsg)
{
    CURL* curl = curl_easy_init();
    if(!curl) {
        errMsg = "Failed to initialize CURL";
        return QJsonObject();
    }

    QByteArray postData = QJsonDocument(data).toJson(QJsonDocument::Compact);

    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData.constData());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, postData.size());

    QJsonObject result = executeRequest(curl, url, headers, errMsg);

    curl_easy_cleanup(curl);
    return result;
}

QJsonObject HttpClient::get(const QString& url, QString& errMsg)
{
    return get(url, nullptr, errMsg);
}

// 带自定义headers的GET请求
QJsonObject HttpClient::get(const QString& url, struct curl_slist* headers, QString& errMsg)
{
    CURL* curl = curl_easy_init();
    if(!curl) {
        errMsg = "Failed to initialize CURL";
        return QJsonObject();
    }

    QJsonObject result = executeRequest(curl, url, headers, errMsg);

    curl_easy_cleanup(curl);
    return result;
}

size_t HttpClient::writeDataToFile(void* ptr, size_t size, size_t nmemb, void* userdata)
{
    QFile* file = static_cast<QFile*>(userdata);
    if (!file || !file->isOpen()) return 0;

    qint64 totalBytes = size * nmemb;
    qint64 written = file->write(static_cast<char*>(ptr), totalBytes);
    if (written != totalBytes) {
        qDebug() << "Write to file failed!";
    }
    return written;
}

bool HttpClient::downloadFile(const QString& url, const QString& saveAsFilePath, QString& errMsg, bool resume, struct curl_slist* headers)
{
    // 检查URL是否为空或格式不正确
    if (url.isEmpty() || !url.startsWith("http://") && !url.startsWith("https://")) {
        errMsg = QString("下载失败: URL格式错误或为空 (%1)").arg(url);
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        errMsg = "Failed to initialize CURL.";
        return false;
    }

    QFile file(saveAsFilePath);

    qint64 startPos = 0;
    if (resume && file.exists()) {
        startPos = file.size(); // 已经下载的字节数
        if (!file.open(QIODevice::Append)) {
            errMsg = QString("无法以追加模式打开文件进行断点续传: %1").arg(saveAsFilePath);
            curl_easy_cleanup(curl);
            return false;
        }
    } else {
        if (!file.open(QIODevice::WriteOnly)) {
            errMsg = QString("无法创建或打开文件进行写入: %1").arg(saveAsFilePath);
            curl_easy_cleanup(curl);
            return false;
        }
    }

    // 设置 URL
    curl_easy_setopt(curl, CURLOPT_URL, url.toUtf8().constData());

    // 设置写入回调
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeDataToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);

    // 🔧 添加超时设置（下载文件30秒超时）
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);  // 整体超时30秒
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);  // 连接超时10秒

    // 支持 HTTPS 证书校验（可根据需要关闭验证，不推荐）
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    // 启用重定向跟随（对应命令行的-L）
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    // 设置最大重定向次数（避免无限重定向，建议10次）
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);

    // CA 证书文件
    QString caCertPath = QCoreApplication::applicationDirPath() + "/resources/cacert.pem";
    curl_easy_setopt(curl, CURLOPT_CAINFO, caCertPath.toUtf8().constData());

    // 设置headers
    if(headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    // 断点续传
    if (resume && startPos > 0) {
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, startPos);
    }

    // 执行请求
    CURLcode res = curl_easy_perform(curl);

    file.close();

    if (res != CURLE_OK) {
        errMsg = QString("下载失败 (CURL error): %1").arg(curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_cleanup(curl);
    return true;
}

bool HttpClient::downloadFileWithProgress(const QString& url, const QString& saveAsFilePath, QString& errMsg, DownloadProgressCallback progressCallback, bool resume, struct curl_slist* headers)
{
    // 检查URL是否为空或格式不正确
    if (url.isEmpty() || !url.startsWith("http://") && !url.startsWith("https://")) {
        errMsg = QString("下载失败: URL格式错误或为空 (%1)").arg(url);
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        errMsg = "Failed to initialize CURL.";
        return false;
    }

    QFile file(saveAsFilePath);

    qint64 startPos = 0;
    if (resume && file.exists()) {
        startPos = file.size();
        if (!file.open(QIODevice::Append)) {
            errMsg = QString("无法以追加模式打开文件进行断点续传: %1").arg(saveAsFilePath);
            curl_easy_cleanup(curl);
            return false;
        }
    } else {
        if (!file.open(QIODevice::WriteOnly)) {
            errMsg = QString("无法创建或打开文件进行写入: %1").arg(saveAsFilePath);
            curl_easy_cleanup(curl);
            return false;
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.toUtf8().constData());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeDataToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);

    // 🔧 添加超时设置（下载文件30秒超时）
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);  // 整体超时30秒
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);  // 连接超时10秒

    // 启用重定向跟随（对应命令行的-L）
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    // 设置最大重定向次数（避免无限重定向，建议10次）
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    QString caCertPath = QCoreApplication::applicationDirPath() + "/resources/cacert.pem";
    curl_easy_setopt(curl, CURLOPT_CAINFO, caCertPath.toUtf8().constData());

    // 设置headers
    if(headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    if (resume && startPos > 0) {
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, startPos);
    }

    // 设置进度回调
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, [](void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) -> int {
        double percent = 0.0;
        if (dltotal > 0)
            percent = (double)dlnow / (double)dltotal * 100.0;

        DownloadProgressCallback *cb = static_cast<DownloadProgressCallback*>(clientp);
        if (cb && *cb)
            (*cb)(percent);

        Q_UNUSED(ultotal)
        Q_UNUSED(ulnow)
        return 0;
    });

    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressCallback);

    CURLcode res = curl_easy_perform(curl);

    // 获取 HTTP 响应状态码
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    live_assistant::Log::info(("HTTP 状态码:" + QString::number(httpCode)).toStdString());
    if (res != CURLE_OK) {
        errMsg = QString("下载失败 (CURL error): %1").arg(curl_easy_strerror(res));
        file.close();
        curl_easy_cleanup(curl);
        return false;
    } else if (httpCode >= 400) {
        errMsg = QString("下载失败 (HTTP 错误): 状态码 %1").arg(httpCode);
        file.close();
        curl_easy_cleanup(curl);
        return false;
    }

    file.close();
    curl_easy_cleanup(curl);
    return true;
}

