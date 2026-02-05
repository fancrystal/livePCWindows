#include "http/download_manager.h"
#include "http/client_service.h"
#include "common/log.h"
#include <QThread>

DownloadManager* DownloadManager::m_instance = nullptr;

DownloadManager* DownloadManager::instance()
{
    if (!m_instance) {
        m_instance = new DownloadManager();
    }
    return m_instance;
}

DownloadManager::DownloadManager(QObject *parent)
    : QObject(parent)
{
}

DownloadManager::~DownloadManager()
{
    stop();
}

void DownloadManager::enqueueDownload(InsertFileItem* fileItem, bool highPriority)
{
    QMutexLocker locker(&m_queueMutex);
    DownloadTask task{fileItem, fileItem->fileId, highPriority};
    if (highPriority) {
        // 优先级高插到队首
        m_downloadQueue.prepend(task);
    } else {
        m_downloadQueue.enqueue(task);
    }
    LOG_INFO(QString("DownloadManager: Enqueue download:%1 Priority:%2")
        .arg(fileItem->fileName).arg(highPriority).toStdString());
}

void DownloadManager::start()
{
    if (m_isRunning) return;
    m_isRunning = true;
    LOG_INFO("DownloadManager: Download scheduler started on thread.");

    while (m_isRunning && !m_stopRequested.load(std::memory_order_relaxed)) {
        processNextDownload();
        
        // 每次循环检查一次停止标志
        for (int i = 0; i < 10; ++i) {
            if (m_stopRequested.load(std::memory_order_relaxed)) {
                break;
            }
            QThread::msleep(10); // 总共100ms，分成10次检查
        }
    }

    m_isRunning = false;
    LOG_INFO("DownloadManager: Download scheduler stopped.");
}

void DownloadManager::stop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    LOG_INFO("DownloadManager: [stop] Stop flag set. Requesting download loop to exit.");
}

void DownloadManager::processNextDownload()
{
    DownloadTask task;
    {
        QMutexLocker locker(&m_queueMutex);
        if (m_downloadQueue.isEmpty()) return;
        task = m_downloadQueue.dequeue();
    }

    if (!task.fileItem) return;

    if (task.fileItem->status == InsertFileStatus::DOWNLOAD_COMPLETED) return;

    const QString fileId = task.fileId;
    const QString localFilePath = task.fileItem->getLocalCachePath();
    
    // 检查本地文件是否已经存在
    QFileInfo fileInfo(localFilePath);
    if (fileInfo.exists() && fileInfo.size() > 0) {
        // 文件已存在，直接标记为下载完成
        LOG_INFO(QString("DownloadManager: File already exists, skipping download:%1").arg(task.fileItem->fileName).toStdString());
        task.fileItem->status = InsertFileStatus::DOWNLOAD_COMPLETED;
        emit downloadStarted(fileId);
        emit downloadProgress(fileId, 100);
        emit downloadFinished(fileId, true, "文件已存在，跳过下载");
        return;
    }

    LOG_INFO(QString("DownloadManager: Processing download:%1 FileID:%2").arg(task.fileItem->fileName).arg(fileId).toStdString());
    emit downloadStarted(fileId);

    // 调用 ClientService 执行真正的下载（含进度回调）
    bool success = false;
    QString errMsg;

    success = ClientService::instance()->downloadFileWithProgress(
        task.fileItem->downloadUrl,
        localFilePath,
        errMsg,
        [&task](double progress) {
            // Lambda 在 downloadFileWithProgress 所在线程执行（可能是子线程）
            int percent = static_cast<int>(progress * 100);
            emit DownloadManager::instance()->downloadProgress(task.fileId, percent);
        }
        );

    if (success) {
        task.fileItem->status = InsertFileStatus::DOWNLOAD_COMPLETED;
    }
    emit downloadFinished(fileId, success, errMsg);
}
