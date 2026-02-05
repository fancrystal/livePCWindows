#ifndef DOWNLOADMANAGER_H
#define DOWNLOADMANAGER_H
#include "app/live_item.h"
#include <QObject>
#include <QMutex>
#include <QQueue>
#include <atomic>

struct DownloadTask
{
    InsertFileItem* fileItem = nullptr;
    QString fileId;
    bool isHighPriority = false;
};

class DownloadManager : public QObject
{
    Q_OBJECT

public:
    static DownloadManager* instance();
    ~DownloadManager();

    // highPriority 是否优先下载（插队到队首）
    void enqueueDownload(InsertFileItem* fileItem, bool highPriority = false);

signals:
    // 下载开始
    void downloadStarted(const QString& fileId);

    // 下载进度更新（0~100）
    void downloadProgress(const QString& fileId, int percent);

    void downloadFinished(const QString& fileId, bool success, const QString& errMsg);

public slots:
    void start();
    void stop();

private:
    explicit DownloadManager(QObject *parent = nullptr);
    void processNextDownload();

    static DownloadManager* m_instance;
    QQueue<DownloadTask> m_downloadQueue;
    QMutex m_queueMutex;

    bool m_isRunning = false;
    std::atomic<bool> m_stopRequested{false};
};

#endif // DOWNLOADMANAGER_H
