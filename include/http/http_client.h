#ifndef HTTPCLIENT_H
#define HTTPCLIENT_H

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QMutex>

extern "C" {
#include <curl/curl.h>
}

// 带进度回调的下载接口
typedef std::function<void(double)> DownloadProgressCallback;

class HttpClient : public QObject
{
    Q_OBJECT
public:
    static HttpClient* instance();
    static void globalInit();
    static void globalCleanup();

    // POST 请求
    QJsonObject post(const QString& url, const QJsonObject& data);
    // 带自定义headers的POST请求
    QJsonObject post(const QString& url, const QJsonObject& data, struct curl_slist* headers);

    // GET 请求
    QJsonObject get(const QString& url);
    // 带自定义headers的GET请求
    QJsonObject get(const QString& url, struct curl_slist* headers);

    // 创建请求头列表
    struct curl_slist* createHeaders();
    
    // 添加请求头
    void addHeader(struct curl_slist** headers, const QString& key, const QString& value);
    
    // 释放请求头
    void freeHeaders(struct curl_slist* headers);

    // 下载文件（支持断点续传和重命名保存）
    // @param saveAsFilePath 最终保存的文件路径，如 D:/downloads/aa.png（可重命名）
    // @param resume 是否启用断点续传
    bool downloadFile(const QString& url, const QString& saveAsFilePath, QString& errMsg, bool resume = false, struct curl_slist* headers = nullptr);

    bool downloadFileWithProgress(const QString& url, const QString& saveAsFilePath, QString& errMsg, DownloadProgressCallback progressCallback, bool resume = false, struct curl_slist* headers = nullptr);
private:
    explicit HttpClient(QObject* parent = nullptr);
    ~HttpClient();

    // 禁止拷贝和赋值
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // CURL 回调函数
    static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp);

    // 执行请求
    QJsonObject executeRequest(CURL* curl, const QString& url, struct curl_slist* headers);

    // CURL 回调函数，将数据写入 QFile
    static size_t writeDataToFile(void* ptr, size_t size, size_t nmemb, void* userdata);

    // 单例指针
    static HttpClient* m_instance;
    static QMutex m_mutex;
    // 全局初始化标志
    static bool m_curlGlobalInitialized;
    static QMutex m_globalMutex;
};

#endif // HTTPCLIENT_H
