#ifndef LIVE_ITEM_H
#define LIVE_ITEM_H

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QVector>

enum class LiveStatus {
    ALL,
    PENDING,
    LIVE,
    ENDED,
    // REPLAY
};

enum class ReviewStatus {
    PENDING,
    APPROVED,
    REJECTED,
};

enum class InsertFileStatus {
    TRANSCODING,
    TRANSCODE_SUCCEEDED,
    TRANSCODE_FAILED,
    DOWNLOADING,
    DOWNLOAD_FAILED,
    DOWNLOAD_COMPLETED,
    PLAYING,
    PLAY_ENDED
};

struct InsertFileItem {
    QString fileId;
    QString downloadUrl;
    QString fileName;
    qint64 durationMs = 0;
    InsertFileStatus status = InsertFileStatus::TRANSCODING;

    // Extra fields returned by the API.
    QString videoRoomId;
    QString videoName;
    QString fileKey;
    int videoType = 0;
    QString videoCoverUrl;
    QString videoDuration;
    QString videoSize;
    int videoHeight = 0;
    int videoWidth = 0;
    QString videoMediaType;
    int videoTransState = 0;
    int verifyStatus = 0;
    QString createName;
    QString fileKeyTransTs;
    QString createTime;

    bool loopEnabled = true;

    QString getLocalCachePath() const {
        QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QString uniqueName = QString("%1_%2").arg(fileId, fileName);
        QString cachePath = QDir::cleanPath(cacheRoot + "/" + uniqueName);
        QDir().mkpath(QFileInfo(cachePath).absolutePath());
        return cachePath;
    }

    bool isDownloaded() const {
        return status == InsertFileStatus::DOWNLOAD_COMPLETED;
    }

    bool isPlayable() const {
        return isDownloaded() || status == InsertFileStatus::PLAYING;
    }
};

struct LiveItem {
    QString liveId;
    QString roomNumber;
    QString title;
    QDateTime createTime;
    QDateTime startTime;
    QDateTime endTime;
    QVector<QString> pushUrl;
    QString horizontalImageUrl;
    QString verticalImageUrl;
    QString liveShareImgUrl;
    int roomType = 0;
    LiveStatus status = LiveStatus::PENDING;
    int roomState = 0;
    int videoScreenMode = 1; // API: 1=landscape, 2=portrait
    int reserveCount = 0;
    int viewCount = 0;
    bool isSelected = false;
    QString canvasOrientation = "landscape";
    QList<InsertFileItem> insertFiles;

    void setVideoScreenMode(int mode) {
        videoScreenMode = mode;
        canvasOrientation = (mode == 2) ? "portrait" : "landscape";
    }

    bool isPortraitMode() const {
        if (videoScreenMode == 1) {
            return false;
        }
        if (videoScreenMode == 2) {
            return true;
        }
        return canvasOrientation.compare("portrait", Qt::CaseInsensitive) == 0;
    }

    QString orientationString() const {
        return isPortraitMode() ? "portrait" : "landscape";
    }
};

inline QString liveStatusToString(LiveStatus status) {
    switch (status) {
        case LiveStatus::PENDING: return "待开播";
        case LiveStatus::LIVE: return "直播中";
        case LiveStatus::ENDED: return "已结束";
        default: return "未知";
    }
}

inline QString liveStatusToApiParam(LiveStatus status) {
    switch (status) {
        case LiveStatus::PENDING: return "PENDING";
        case LiveStatus::LIVE: return "LIVE";
        case LiveStatus::ENDED: return "ENDED";
        default: return "ALL";
    }
}

inline QString insertFileStatusToString(InsertFileStatus status) {
    switch (status) {
    case InsertFileStatus::TRANSCODING: return "转码中";
    case InsertFileStatus::TRANSCODE_SUCCEEDED: return "转码成功";
    case InsertFileStatus::TRANSCODE_FAILED: return "转码失败";
    case InsertFileStatus::DOWNLOADING: return "下载中";
    case InsertFileStatus::DOWNLOAD_COMPLETED: return "下载完成";
    case InsertFileStatus::DOWNLOAD_FAILED: return "下载失败";
    case InsertFileStatus::PLAYING: return "播放中";
    case InsertFileStatus::PLAY_ENDED: return "播放结束";
    default: return "未知状态";
    }
}

#endif // LIVE_ITEM_H
