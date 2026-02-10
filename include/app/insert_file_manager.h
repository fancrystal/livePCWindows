#ifndef INSERT_FILE_MANAGER_H
#define INSERT_FILE_MANAGER_H

#include <QObject>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QQueue>
#include <memory>
#include <atomic>
#include "app/insert_file_item.h"

class InsertFileManager : public QObject {
    Q_OBJECT
public:
    static InsertFileManager* instance();

    // ========== API 接口 ==========

    /**
     * @brief 刷新插播文件列表
     * @param roomId 直播间ID
     */
    void refreshInsertFiles(const QString& roomId);

    /**
     * @brief 设置直播间信息（用于后续API调用）
     * @param sassUrl 服务地址
     * @param userId 用户ID
     * @param token 认证令牌
     */
    void setLiveInfo(const QString& sassUrl, const QString& userId, const QString& token);

    /**
     * @brief 开始下载插播文件
     * @param fileId 文件ID
     */
    void startDownload(const QString& fileId);

    /**
     * @brief 开始下载（重载）
     * @param item 插播文件项
     */
    void startDownload(std::shared_ptr<InsertFileItem> item);

    /**
     * @brief 停止下载
     */
    void stopDownload(const QString& fileId);

    /**
     * @brief 获取所有插播文件
     */
    QList<std::shared_ptr<InsertFileItem>> getAllFiles() const;

    /**
     * @brief 获取指定文件
     */
    std::shared_ptr<InsertFileItem> getFile(const QString& fileId) const;

    /**
     * @brief 检查文件是否可播放
     */
    bool isPlayable(const QString& fileId) const;

    /**
     * @brief 获取已下载的文件列表
     */
    QList<std::shared_ptr<InsertFileItem>> getDownloadedFiles() const;

    /**
     * @brief 开始后台下载队列
     */
    void startDownloadQueue();

    /**
     * @brief 停止后台下载队列
     */
    void stopDownloadQueue();

signals:
    // 列表更新信号
    void filesUpdated();

    // 下载进度信号
    void downloadProgress(const QString& fileId, int percent);
    void downloadFinished(const QString& fileId, bool success, const QString& message);

    // 错误信号
    void errorOccurred(const QString& fileId, const QString& message);

private:
    InsertFileManager(QObject* parent = nullptr);
    ~InsertFileManager();

    // 内部下载处理
    void processDownloadQueue();

    // 数据成员
    QList<std::shared_ptr<InsertFileItem>> insert_files_;
    QQueue<std::shared_ptr<InsertFileItem>> download_queue_;
    QMap<QString, std::shared_ptr<InsertFileItem>> file_map_;
    mutable QMutex files_mutex_;
    QMutex queue_mutex_;

    // 直播间信息
    QString sass_url_;
    QString user_id_;
    QString token_;
    QString current_room_id_;
    mutable QMutex live_info_mutex_;

    // 下载状态
    std::atomic<bool> download_running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> is_downloading_{false};
    QString current_downloading_file_id_;
};

#endif // INSERT_FILE_MANAGER_H
