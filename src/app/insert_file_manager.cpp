#include "app/insert_file_manager.h"
#include "http/client_service.h"
#include "common/log.h"
#include <QThread>
#include <QFile>
#include <QtConcurrent/QtConcurrent>
#include <algorithm>
#include <cmath>

// InsertFileItem 已在 http/live_item.h 中定义

namespace {
qint64 parseVideoSizeToBytes(const QString& videoSize)
{
    const QString normalized = videoSize.trimmed().toUpper();
    if (normalized.isEmpty()) {
        return 0;
    }

    int suffixIndex = 0;
    while (suffixIndex < normalized.size() &&
           (normalized.at(suffixIndex).isDigit() || normalized.at(suffixIndex) == '.')) {
        ++suffixIndex;
    }

    bool ok = false;
    double value = normalized.left(suffixIndex).trimmed().toDouble(&ok);
    if (!ok || value <= 0.0) {
        return 0;
    }

    if (normalized.contains("GB")) {
        value *= 1024.0 * 1024.0 * 1024.0;
    } else if (normalized.contains("MB")) {
        value *= 1024.0 * 1024.0;
    } else if (normalized.contains("KB")) {
        value *= 1024.0;
    }

    return static_cast<qint64>(std::llround(value));
}

bool isCacheSizeValid(qint64 actualBytes, const QString& expectedSizeText)
{
    if (actualBytes <= 0) {
        return false;
    }

    const qint64 expectedBytes = parseVideoSizeToBytes(expectedSizeText);
    if (expectedBytes <= 0) {
        return actualBytes > 0;
    }

    const qint64 diff = actualBytes > expectedBytes ? actualBytes - expectedBytes : expectedBytes - actualBytes;
    const qint64 tolerance = (std::max)(static_cast<qint64>(expectedBytes * 0.05), static_cast<qint64>(16 * 1024));
    return diff <= tolerance;
}
}

InsertFileManager* InsertFileManager::instance() {
    static InsertFileManager instance;
    return &instance;
}

InsertFileManager::InsertFileManager(QObject* parent)
    : QObject(parent) {
}

InsertFileManager::~InsertFileManager() {
    stopDownloadQueue();
}

void InsertFileManager::setLiveInfo(const QString& sassUrl, const QString& userId, const QString& token) {
    QMutexLocker locker(&live_info_mutex_);
    sass_url_ = sassUrl;
    user_id_ = userId;
    token_ = token;
    LOG_INFO(QString("InsertFileManager::setLiveInfo - sassUrl: %1, userId: %2, token: %3")
             .arg(sassUrl).arg(userId).arg(token.isEmpty() ? "empty" : "***").toStdString());
}

void InsertFileManager::refreshInsertFiles(const QString& roomId) {
    LOG_INFO(QString("InsertFileManager::refreshInsertFiles ENTER - roomId: %1").arg(roomId).toStdString());

    QMutexLocker locker(&live_info_mutex_);
    QString sassUrl = sass_url_;
    QString userId = user_id_;
    QString token = token_;
    locker.unlock();

    LOG_INFO(QString("InsertFileManager - sassUrl: %1, userId: %2, roomId: %3")
             .arg(sassUrl).arg(userId).arg(roomId).toStdString());

    if (sassUrl.isEmpty() || userId.isEmpty() || token.isEmpty()) {
        LOG_ERROR("InsertFileManager: Live info not set");
        emit errorOccurred("", "直播间信息未设置");
        return;
    }

    current_room_id_ = roomId;

    QList<InsertFileItem> fileList;
    int totalCount = 0;
    QString errMessage;

    // 调用API获取插播视频列表
    // 使用原项目的参数: originType=1, videoTransState=2, verifyStatus=1, pageNum=1, pageSize=20
    LOG_INFO(QString("InsertFileManager: Calling getInsertVideolist - sassUrl: %1, roomId: %2")
             .arg(sassUrl).arg(roomId).toStdString());

    // 尝试 originType=1 (视频上传)
    bool success = ClientService::instance()->getInsertVideolist(
        sassUrl, userId, token, roomId, "", 2, 1, 1, 1, 20,
        fileList, totalCount, errMessage
    );

    LOG_INFO(QString("InsertFileManager: getInsertVideolist returned %1, totalCount: %2, errMessage: %3")
             .arg(success).arg(totalCount).arg(errMessage).toStdString());

    if (!success) {
        LOG_ERROR("InsertFileManager: Failed to get insert video list: " + errMessage.toStdString());
        emit errorOccurred("", "获取插播视频列表失败: " + errMessage);
        return;
    }

    // 更新文件列表
    {
        QMutexLocker fileLocker(&files_mutex_);
        insert_files_.clear();
        file_map_.clear();

        for (const auto& item : fileList) {
            auto sharedItem = std::make_shared<InsertFileItem>(item);
            insert_files_.append(sharedItem);
            file_map_[sharedItem->fileId] = sharedItem;

            // 检查文件是否已下载
            QString localPath = sharedItem->getLocalCachePath();
            QFile file(localPath);
            if (file.exists() && isCacheSizeValid(file.size(), sharedItem->videoSize)) {
                sharedItem->status = InsertFileStatus::DOWNLOAD_COMPLETED;
                LOG_INFO("InsertFileManager: File already downloaded: " + sharedItem->fileName.toStdString());
            } else if (file.exists()) {
                LOG_WARNING("InsertFileManager: Removing invalid cache for " +
                            sharedItem->fileName.toStdString() +
                            ", local_size=" + std::to_string(file.size()) +
                            ", expected_size=" + sharedItem->videoSize.toStdString());
                QFile::remove(localPath);
            }
        }
    }

    LOG_INFO("InsertFileManager: Loaded " + std::to_string(insert_files_.size()) + " insert files");
    emit filesUpdated();

    // 自动开始下载未下载的文件
    for (const auto& item : insert_files_) {
        if (item->status != InsertFileStatus::DOWNLOAD_COMPLETED) {
            startDownload(item);
        }
    }
}

void InsertFileManager::startDownload(const QString& fileId) {
    auto item = getFile(fileId);
    if (item) {
        startDownload(item);
    }
}

void InsertFileManager::startDownload(std::shared_ptr<InsertFileItem> item) {
    if (!item) return;

    // 如果已经下载完成，不需要再下载
    if (item->status == InsertFileStatus::DOWNLOAD_COMPLETED) {
        return;
    }

    // 如果正在下载中，也不需要重复添加
    if (item->status == InsertFileStatus::DOWNLOADING) {
        return;
    }

    {
        QMutexLocker locker(&queue_mutex_);
        // 检查是否已在队列中
        for (const auto& queuedItem : download_queue_) {
            if (queuedItem->fileId == item->fileId) {
                return;
            }
        }

        item->status = InsertFileStatus::DOWNLOADING;
        download_queue_.enqueue(item);
    }

    LOG_INFO("InsertFileManager: Enqueued download for " + item->fileName.toStdString());

    // 如果下载队列未运行，启动它
    if (!download_running_.load()) {
        startDownloadQueue();
    }
}

void InsertFileManager::stopDownload(const QString& fileId) {
    QMutexLocker locker(&queue_mutex_);

    // 如果正在下载这个文件，设置停止标志
    if (current_downloading_file_id_ == fileId) {
        stop_requested_.store(true);
    }

    // 从队列中移除
    QQueue<std::shared_ptr<InsertFileItem>> newQueue;
    while (!download_queue_.isEmpty()) {
        auto item = download_queue_.dequeue();
        if (item->fileId != fileId) {
            newQueue.enqueue(item);
        } else {
            item->status = InsertFileStatus::TRANSCODE_SUCCEEDED;
        }
    }
    download_queue_ = newQueue;
}

QList<std::shared_ptr<InsertFileItem>> InsertFileManager::getAllFiles() const {
    QMutexLocker locker(&files_mutex_);
    return insert_files_;
}

std::shared_ptr<InsertFileItem> InsertFileManager::getFile(const QString& fileId) const {
    QMutexLocker locker(&files_mutex_);
    return file_map_.value(fileId, nullptr);
}

bool InsertFileManager::isPlayable(const QString& fileId) const {
    auto item = getFile(fileId);
    return item && (item->status == InsertFileStatus::DOWNLOAD_COMPLETED ||
                    item->status == InsertFileStatus::PLAYING);
}

QList<std::shared_ptr<InsertFileItem>> InsertFileManager::getDownloadedFiles() const {
    QMutexLocker locker(&files_mutex_);
    QList<std::shared_ptr<InsertFileItem>> result;
    for (const auto& item : insert_files_) {
        if (item->status == InsertFileStatus::DOWNLOAD_COMPLETED) {
            result.append(item);
        }
    }
    return result;
}

void InsertFileManager::startDownloadQueue() {
    if (download_running_.exchange(true)) {
        return; // 已经在运行
    }

    stop_requested_.store(false);

    // 在后台线程中处理下载队列
    QtConcurrent::run([this]() {
        processDownloadQueue();
    });

    LOG_INFO("InsertFileManager: Download queue started");
}

void InsertFileManager::stopDownloadQueue() {
    stop_requested_.store(true);
    download_running_.store(false);

    // 等待当前下载完成
    while (is_downloading_.load()) {
        QThread::msleep(100);
    }

    LOG_INFO("InsertFileManager: Download queue stopped");
}

void InsertFileManager::processDownloadQueue() {
    QMutexLocker locker(&live_info_mutex_);
    QString sassUrl = sass_url_;
    QString userId = user_id_;
    QString token = token_;
    locker.unlock();

    while (!stop_requested_.load()) {
        std::shared_ptr<InsertFileItem> item;

        {
            QMutexLocker queueLocker(&queue_mutex_);
            if (download_queue_.isEmpty()) {
                break; // 队列为空，退出循环
            }
            item = download_queue_.dequeue();
        }

        if (!item) continue;

        is_downloading_.store(true);
        current_downloading_file_id_ = item->fileId;

        QString localPath = item->getLocalCachePath();
        QString errMsg;

        LOG_INFO("InsertFileManager: Starting download for " + item->fileName.toStdString() +
                 " from " + item->downloadUrl.toStdString());

        // 使用进度回调的下载
        bool success = ClientService::instance()->downloadFileWithProgress(
            item->downloadUrl,
            localPath,
            errMsg,
            [this, item](int percent) {
                emit downloadProgress(item->fileId, percent);
            },
            true // 支持断点续传
        );

        if (stop_requested_.load()) {
            is_downloading_.store(false);
            break;
        }

        if (success) {
            item->status = InsertFileStatus::DOWNLOAD_COMPLETED;
            LOG_INFO("InsertFileManager: Download completed for " + item->fileName.toStdString());
            emit downloadFinished(item->fileId, true, "下载完成");
        } else {
            item->status = InsertFileStatus::DOWNLOAD_FAILED;
            LOG_ERROR("InsertFileManager: Download failed for " + item->fileName.toStdString() +
                      ": " + errMsg.toStdString());
            emit downloadFinished(item->fileId, false, errMsg);
            emit errorOccurred(item->fileId, "下载失败: " + errMsg);
        }

        is_downloading_.store(false);
        current_downloading_file_id_.clear();
    }

    download_running_.store(false);
    LOG_INFO("InsertFileManager: Download queue processing finished");
}
