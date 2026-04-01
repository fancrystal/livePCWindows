#ifndef LIVE_ITEM_H
#define LIVE_ITEM_H

#include <QString>
#include <QDateTime>
#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

// 直播状态枚举
enum class LiveStatus {
    ALL,       // 全部
    PENDING,   // 待开播
    LIVE,      // 直播中
    ENDED,     // 已结束
    // REPLAY     // 回放中
};

// 审核状态枚举
enum class ReviewStatus {
    PENDING,   // 待审核
    APPROVED,  // 审核通过
    REJECTED,  // 驳回
};

// 插播文件状态枚举
enum class InsertFileStatus {
    TRANSCODING,          // 转码中
    TRANSCODE_SUCCEEDED,  // 转码成功
    TRANSCODE_FAILED,     // 转码失败
    DOWNLOADING,          // 下载中
    DOWNLOAD_FAILED,      // 下载失败
    DOWNLOAD_COMPLETED,   // 下载完成
    PLAYING,              // 播放中
    PLAY_ENDED            // 播放结束
};

// 插播文件结构体
struct InsertFileItem {
    QString fileId;                                             // 文件唯一ID（素材库中的UUID）
    QString downloadUrl;                                        // 文件下载路径
    QString fileName;                                           // 文件名（如"盘锦河蟹基地.mp4"）
    qint64 durationMs;                                          // 时长（毫秒）
    InsertFileStatus status = InsertFileStatus::TRANSCODING;    // 文件状态
    
    // API返回的额外字段
    QString videoRoomId;                                        // 视频房间ID
    QString videoName;                                          // 视频名称（对应fileName）
    QString fileKey;                                            // 文件Key
    int videoType = 0;                                           // 视频类型
    QString videoCoverUrl;                                      // 视频封面URL
    QString videoDuration;                                      // 视频时长（字符串格式）
    QString videoSize;                                          // 视频大小
    int videoHeight = 0;                                         // 视频高度
    int videoWidth = 0;                                          // 视频宽度
    QString videoMediaType;                                     // 视频媒体类型
    int videoTransState = 0;                                     // 视频转码状态
    int verifyStatus = 0;                                        // 审核状态
    QString createName;                                         // 创建人姓名
    QString fileKeyTransTs;                                     // 文件Key转码时间戳
    QString createTime;                                         // 创建时间

    // 循环播放设置（默认开启）
    bool loopEnabled = true;                                   // 是否循环播放
    
    QString getLocalCachePath() const {
        // 1. 获取系统推荐的缓存根目录
        QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        // 2. 使用 fileId + fileName 确保唯一性，避免同名文件冲突（如多个"竖屏.mp4"）
        QString uniqueName = QString("%1_%2").arg(fileId).arg(fileName);
        QString cachePath = QDir::cleanPath(cacheRoot + "/" + uniqueName);
        // 3. 确保目录存在（不存在则创建，避免写文件时因目录不存在失败）
        QDir().mkpath(QFileInfo(cachePath).absolutePath());
        return cachePath;
    }

    // 检查是否已下载
    bool isDownloaded() const {
        return status == InsertFileStatus::DOWNLOAD_COMPLETED;
    }

    // 检查是否可播放
    bool isPlayable() const {
        return isDownloaded() || status == InsertFileStatus::PLAYING;
    }
};

// 直播列表项结构体
struct LiveItem {
    QString liveId;                     // 直播ID（关键标识）
    QString title;                      // 直播标题
    QDateTime createTime;               // 创建时间
    QDateTime startTime;                // 开播时间
    QDateTime endTime;                  // 结束时间
    QVector<QString> pushUrl;           // 推流地址
    QString type;                       // 直播类型（视频直播/回放直播）
    LiveStatus status;                  // 直播状态
    int reserveCount;                   // 预约人数
    int viewCount;                      // 观看人数
    bool isSelected = false;            // 是否被勾选
    QString canvasOrientation;          // 画布方向: "landscape"(横屏16:9) | "portrait"(竖屏9:16), 默认横屏
    QList<InsertFileItem> insertFiles;  // 该直播的插播文件列表

    // 便捷方法：检查是否为竖屏模式
    bool isPortraitMode() const {
        return canvasOrientation.toLower() == "portrait";
    }
};

// 状态转字符串（用于UI显示和接口参数）
inline QString liveStatusToString(LiveStatus status) {
    switch (status) {
        case LiveStatus::PENDING: return "待开播";
        case LiveStatus::LIVE: return "直播中";
        case LiveStatus::ENDED: return "已结束";
        // case LiveStatus::REPLAY: return "回放中";
        default: return "未知";
    }
}

// 状态转接口参数（用于请求筛选）
inline QString liveStatusToApiParam(LiveStatus status) {
    switch (status) {
        case LiveStatus::PENDING: return "PENDING";
        case LiveStatus::LIVE: return "LIVE";
        case LiveStatus::ENDED: return "ENDED";
        // case LiveStatus::REPLAY: return "REPLAY";
        default: return "ALL";
    }
}

// 插播文件状态转换（同步枚举修改）
inline QString insertFileStatusToString(InsertFileStatus status) {
    switch (status) {
    case InsertFileStatus::TRANSCODING:          return "转码中";
    case InsertFileStatus::TRANSCODE_SUCCEEDED:   return "转码成功";
    case InsertFileStatus::TRANSCODE_FAILED:     return "转码失败";
    case InsertFileStatus::DOWNLOADING:          return "下载中";
    case InsertFileStatus::DOWNLOAD_COMPLETED:   return "下载完成";
    case InsertFileStatus::DOWNLOAD_FAILED:      return "下载失败";
    case InsertFileStatus::PLAYING:              return "播放中";
    case InsertFileStatus::PLAY_ENDED:           return "播放结束";
    default: return "未知状态";
    }
}

// const QString LOGIN_URL = "https://account-api.lxi-tech.com";
// // const QString LOGIN_URL = "http://192.168.1.179:9082";
// const QString LIVE_URL = "https://livesaas-api.lxi-tech.com:";
// const QString ENCRYPTION_KEY = "h7kP9xR2vLmQwE5t";

#endif // LIVE_ITEM_H
