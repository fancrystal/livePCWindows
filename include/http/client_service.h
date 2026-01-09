// client_service.h
#ifndef CLIENT_SERVICE_H
#define CLIENT_SERVICE_H
#include <QString>
#include <QMutex>
#include <QList>
#include "live_item.h"
#include "http_client.h"

class ClientService
{
public:
    static ClientService* instance();

    bool login(const QString &logUrl, const QString &key, const QString &username, const QString &password, QString &userID, QString &token);
#if 0
    bool getStreamAddresses(const QString &userId, const QString &token, QString &rtmpURL, QString &srtURL,
                            QString &liveStartTime, QString &liveEndTime, QString &liveID, QString &errMessage);
    bool stopLiving(const QString &userId, QString &liveID, const QString &token, QString &errMessage);
#endif
    bool getLiveList(const QString &sassUrl, const QString& userId, const QString& token, int pageNum, int pageSize, int liveType,
                     int liveStreamStatus, int reviewStatus, QList<LiveItem>& liveList, int& totalCount, QString& errMessage);
    bool getInsertVideolist(const QString& sassUrl, const QString& userId, const QString& token, const QString& roomId,
                           const QString& videoName, int videoTransState, int verifyStatus, int originType,
                           int pageNum, int pageSize, QList<InsertFileItem>& insertFileList, int& totalCount, QString& errMessage);
    bool getInsertFile(const QString& sassUrl, const QString& userId, const QString& token, const QString& videoRoomId,
                      const QString& roomInfoId, InsertFileItem& fileItem, QString& errMessage);
    bool genOnceLoginKey(const QString& baseUrl, const QString& userId, const QString& token, QString& loginKey, QString& errMessage);
    bool downloadFile(const QString& url, const QString& saveAsFilePath, QString& errMsg, bool resume = true);
    bool downloadFileWithProgress(const QString& url, const QString& saveAsFilePath, QString& errMsg, DownloadProgressCallback progressCallback, bool resume = true);

private:
    ClientService() = default;
    ~ClientService() = default;

    ClientService(const ClientService&) = delete;
    ClientService(ClientService&&) = delete;
    ClientService& operator=(const ClientService&) = delete;
    ClientService& operator=(ClientService&&) = delete;
    // 解析直播列表JSON
    void parseLiveListJson(const QJsonObject& json, QList<LiveItem>& liveList, int& totalCount);
    
    // 解析单个插播视频JSON
    void parseInsertFileJson(const QJsonObject& recordJson, InsertFileItem& fileItem);
    // 解析插播视频列表JSON
    void parseInsertVideolistJson(const QJsonObject& json, QList<InsertFileItem>& insertFileList, int& totalCount);
    void parseInsertFile(const QJsonObject& jsonArr, InsertFileItem& fileItem);

    static ClientService* m_instance;
    static QMutex m_mutex;
};

#endif // CLIENT_SERVICE_H
