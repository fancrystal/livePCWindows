#include "http/client_service.h"
#include "http/http_client.h"
#include "app/encryption_utils.h"
#include "common/log.h"
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDateTime>
#include <QStringList>
#include <QUrl>

namespace {
QString makeAuthorizationHeader(const QString& token)
{
    QString normalized = token.trimmed();
    if (normalized.startsWith("Bearer ", Qt::CaseInsensitive)) {
        normalized = normalized.mid(7).trimmed();
    }
    return QString("Bearer %1").arg(normalized);
}

QDateTime parseApiDateTime(const QString& value)
{
    static const QStringList kFormats = {
        QStringLiteral("yyyy-MM-dd HH:mm:ss"),
        QStringLiteral("yyyy-MM-dd HH:mm")
    };

    const QString trimmed = value.trimmed();
    for (const QString& format : kFormats) {
        QDateTime parsed = QDateTime::fromString(trimmed, format);
        if (parsed.isValid()) {
            return parsed;
        }
    }

    QDateTime isoParsed = QDateTime::fromString(trimmed, Qt::ISODate);
    return isoParsed;
}

QString originFromUrl(const QString& value, bool keepPort)
{
    QUrl url(value.trimmed());
    if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty()) {
        return QString();
    }

    QString origin = QString("%1://%2").arg(url.scheme(), url.host());
    if (keepPort && url.port() > 0) {
        origin += QString(":%1").arg(url.port());
    }
    return origin;
}

QString buildDownloadUrlFromFileKey(const QString& baseUrl, const QString& fileKey, const QString& assetUrl)
{
    const QString trimmedFileKey = fileKey.trimmed();
    if (trimmedFileKey.startsWith("http://") || trimmedFileKey.startsWith("https://")) {
        return trimmedFileKey;
    }

    QString normalizedBaseUrl = originFromUrl(assetUrl, true);
    if (normalizedBaseUrl.isEmpty()) {
        normalizedBaseUrl = originFromUrl(baseUrl, false);
    }

    QString normalizedFileKey = trimmedFileKey;
    while (normalizedFileKey.startsWith('/')) {
        normalizedFileKey.remove(0, 1);
    }

    if (normalizedBaseUrl.isEmpty()) {
        return normalizedFileKey;
    }
    return QString("%1/%2").arg(normalizedBaseUrl, normalizedFileKey);
}
}

ClientService* ClientService::m_instance = nullptr;
QMutex ClientService::m_mutex;

ClientService* ClientService::instance()
{
    QMutexLocker locker(&m_mutex);
    if (!m_instance) {
        m_instance = new ClientService();
    }
    return m_instance;
}

#if 0
bool ClientService::getStreamAddresses(const QString &userId, const QString &token, QString &rtmpURL, QString &srtURL,
                                       QString &liveStartTime, QString &liveEndTime, QString &liveID, QString &errMessage)
{
    QString encryptedUserId = EncryptionUtils::encryptAES128_CBC(userId, ENCRYPTION_KEY);
    if (encryptedUserId.isEmpty()) {
        LOG_WARNING("Failed to encrypt user ID");
        return false;
    }

    QString url = QString("%1/client/getList?encryptedUserId=%2").arg(LIVE_URL).arg(encryptedUserId);

    HttpClient* client = HttpClient::instance();
    client->clearHeaders();
    client->setGlobalHeader("Authorization", QString("Bearer %1").arg(token));

    QJsonObject response = client->get(url);

    if(response["code"].toInt() == 200 && response["success"].toBool()) {
        QJsonObject data = response["data"].toObject();
        rtmpURL = data["rtmpUrl"].toString();
        srtURL = data["srtUrl"].toString();
        liveStartTime = data["livestreamStartTime"].toString();
        liveEndTime = data["livestreamEndTime"].toString();
        liveID = data["liveId"].toString();
        return true;
    }

    errMessage = response["msg"].toString();
    LOG_WARNING("Failed to get stream addresses:" + response["msg"].toString());
    return false;
}

bool ClientService::stopLiving(const QString &userId, QString &liveID, const QString &token, QString &errMessage)
{
    QString encryptedUserId = EncryptionUtils::encryptAES128_CBC(userId, ENCRYPTION_KEY);
    if (encryptedUserId.isEmpty()) {
        LOG_WARNING("Failed to encrypt user ID");
        return false;
    }

    QString encryptedLiveId = EncryptionUtils::encryptAES128_CBC(liveID, ENCRYPTION_KEY);
    if (encryptedLiveId.isEmpty()) {
        LOG_WARNING("Failed to encrypt live ID");
        return false;
    }

    QString url = QString("%1/client/stopLiveStream?encryptedUserId=%2&liveId=%3").arg(LIVE_URL).arg(encryptedUserId).arg(encryptedLiveId);

    HttpClient* client = HttpClient::instance();
    client->clearHeaders();
    client->setGlobalHeader("Authorization", QString("Bearer %1").arg(token));

    QJsonObject response = client->get(url);

    if(response["code"].toInt() == 200 && response["success"].toBool()) {
        LOG_INFO("The live broadcast has ended, notify the backend, token:" + token + ",userID: " + userId);
        return true;
    }

    errMessage = response["msg"].toString();
    LOG_WARNING("Failed to stopLiving:" + response["msg"].toString());
    return false;
}

#endif
bool ClientService::getLiveList(const QString &sassUrl, const QString& userId, const QString& token, int pageNum,
                                int pageSize, int liveType, int liveStreamStatus, int reviewStatus,
                                QList<LiveItem>& liveList, int& totalCount, QString& errMessage)
{
    errMessage.clear();
    liveList.clear();
    totalCount = 0;

    QJsonObject reqData;
    reqData["pageNum"] = pageNum;
    reqData["pageSize"] = pageSize;
    reqData["roomType"] = liveType;
    reqData["roomState"] = liveStreamStatus;
    // reqData["roomState"] = 3;
    reqData["roomVerifyStatusAdmin"] = reviewStatus;

    QString url = QString("%1/livesaas/ListActivityAPI").arg(sassUrl);
    HttpClient* client = HttpClient::instance();
    LOG_DEBUG(QString("获取直播列表请求URL: %1").arg(url).toStdString());
    
    struct curl_slist* headers = client->createHeaders();
    client->addHeader(&headers, "Authorization", makeAuthorizationHeader(token));
    client->addHeader(&headers, "Content-Type", "application/json");
    
    
    QString httpErrMsg;
    QJsonObject response = client->post(url, reqData, headers, httpErrMsg);
    client->freeHeaders(headers);

    
    if (!httpErrMsg.isEmpty()) {
        errMessage = httpErrMsg;
        LOG_WARNING(QString("获取直播列表HTTP请求失败: %1").arg(errMessage).toStdString());
        return false;
    }

    QJsonDocument doc(response);
    LOG_DEBUG(QString("获取直播列表响应: %1").arg(QString(doc.toJson(QJsonDocument::Compact))).toStdString());
    
    if (response["code"].toInt() != 200) {
        errMessage = response["msg"].toString();
        LOG_WARNING(QString("获取直播列表失败: %1").arg(errMessage).toStdString());
        return false;
    }

    
    parseLiveListJson(response["data"].toObject(), liveList, totalCount);
    return true;
}

bool ClientService::downloadFile(const QString& url, const QString& saveAsFilePath, QString& errMsg, bool resume)
{
    HttpClient* client = HttpClient::instance();
    return client->downloadFile(url, saveAsFilePath, errMsg, resume);
}

bool ClientService::downloadFileWithProgress(const QString& url, const QString& saveAsFilePath, QString& errMsg, DownloadProgressCallback progressCallback, bool resume)
{
    HttpClient* client = HttpClient::instance();
    return client->downloadFileWithProgress(url, saveAsFilePath, errMsg, progressCallback, resume);
}

bool ClientService::getInsertVideolist(const QString& sassUrl, const QString& userId, const QString& token, const QString& roomId,
                                       const QString& videoName, int videoTransState, int verifyStatus, int originType,
                                       int pageNum, int pageSize, QList<InsertFileItem>& insertFileList, int& totalCount, QString& errMessage)
{
    errMessage.clear();
    insertFileList.clear();
    totalCount = 0;

    LOG_INFO(QString("getInsertVideolist ENTER - sassUrl: %1, roomId: %2, videoName: %3, transState: %4, verifyStatus: %5, originType: %6")
             .arg(sassUrl).arg(roomId).arg(videoName).arg(videoTransState).arg(verifyStatus).arg(originType).toStdString());

    
    QJsonObject reqData;
    reqData["roomInfoId"] = roomId;
    reqData["videoName"] = videoName;
    reqData["videoTransState"] = videoTransState;
    reqData["verifyStatus"] = verifyStatus;
    reqData["originType"] = originType;
    reqData["pageNum"] = pageNum;
    reqData["pageSize"] = pageSize;

    
    QString url = QString("%1/livesaas/ListVideoRoom").arg(sassUrl);
    HttpClient* client = HttpClient::instance();

    LOG_INFO(QString("getInsertVideolist - Request URL: %1").arg(url).toStdString());

    
    QJsonDocument reqDoc(reqData);
    LOG_INFO(QString("getInsertVideolist - Request body: %1").arg(reqDoc.toJson(QJsonDocument::Compact)).toStdString());

    
    struct curl_slist* headers = client->createHeaders();
    client->addHeader(&headers, "Authorization", makeAuthorizationHeader(token));
    client->addHeader(&headers, "Content-Type", "application/json");

    
    QString httpErrMsg;
    QJsonObject response = client->post(url, reqData, headers, httpErrMsg);
    client->freeHeaders(headers);

    
    if (!httpErrMsg.isEmpty()) {
        errMessage = httpErrMsg;
        LOG_WARNING(QString("获取插播视频列表HTTP请求失败: %1").arg(errMessage).toStdString());
        return false;
    }

    
    QJsonDocument doc(response);
    QString responseStr = doc.toJson(QJsonDocument::Compact);
    LOG_INFO(QString("getInsertVideolist - Response: %1").arg(responseStr).toStdString());

    
    if (response["code"].toInt() != 200) {
        errMessage = response["msg"].toString();
        LOG_WARNING(QString("获取插播视频列表失败: %1").arg(errMessage).toStdString());
        return false;
    }

    
    parseInsertVideolistJson(response["data"].toObject(), insertFileList, totalCount, sassUrl);

    LOG_INFO(QString("getInsertVideolist EXIT - Parsed %1 files, totalCount: %2").arg(insertFileList.size()).arg(totalCount).toStdString());

    return true;
}


void ClientService::parseInsertFileJson(const QJsonObject& recordJson, InsertFileItem& fileItem, const QString& sassUrl)
{
    LOG_INFO(QString("parseInsertFileJson ENTER - recordJson keys: %1")
             .arg(QStringList(recordJson.keys()).join(",")).toStdString());

    
    fileItem.fileId = recordJson["videoRoomId"].toString();
    fileItem.fileName = recordJson["videoName"].toString();
    fileItem.videoRoomId = recordJson["videoRoomId"].toString();
    fileItem.videoName = recordJson["videoName"].toString();
    fileItem.fileKey = recordJson["fileKey"].toString();

    LOG_INFO(QString("parseInsertFileJson - fileId: %1, fileName: %2, fileKey: %3")
             .arg(fileItem.fileId).arg(fileItem.fileName).arg(fileItem.fileKey).toStdString());

    
    bool hasTranscodingUrl = recordJson.contains("transcodingFileMp4Url") && !recordJson["transcodingFileMp4Url"].toString().isEmpty();
    LOG_INFO(QString("parseInsertFileJson - has transcodingFileMp4Url: %1").arg(hasTranscodingUrl).toStdString());

    
    if (hasTranscodingUrl) {
        fileItem.downloadUrl = recordJson["transcodingFileMp4Url"].toString();
        LOG_INFO(QString("parseInsertFileJson - downloadUrl from transcodingFileMp4Url: %1")
                 .arg(fileItem.downloadUrl).toStdString());
    } else if (recordJson.contains("fileKey") && !recordJson["fileKey"].toString().isEmpty()) {
        fileItem.downloadUrl = buildDownloadUrlFromFileKey(
            sassUrl,
            recordJson["fileKey"].toString(),
            recordJson["videoCoverUrl"].toString());
        LOG_INFO(QString("parseInsertFileJson - downloadUrl constructed: %1")
                 .arg(fileItem.downloadUrl).toStdString());
    } else {
        LOG_WARNING("parseInsertFileJson - No downloadUrl available!");
    }
    fileItem.videoType = recordJson["videoType"].toInt();
    fileItem.videoCoverUrl = recordJson["videoCoverUrl"].toString();
    fileItem.videoDuration = recordJson["videoDuration"].toString();
    fileItem.videoSize = recordJson["videoSize"].toString();
    fileItem.videoHeight = recordJson["videoHeight"].toInt();
    fileItem.videoWidth = recordJson["videoWidth"].toInt();
    fileItem.videoMediaType = recordJson["videoMediaType"].toString();
    fileItem.videoTransState = recordJson["videoTransState"].toInt();
    fileItem.verifyStatus = recordJson["verifyStatus"].toInt();
    fileItem.createName = recordJson["createName"].toString();
    fileItem.fileKeyTransTs = recordJson["fileKeyTransTs"].toString();
    fileItem.createTime = recordJson["createTime"].toString();

    
    int transState = recordJson["videoTransState"].toInt();
    if (transState == 1) {
        fileItem.status = InsertFileStatus::TRANSCODING;  
    } else if (transState == 2) {
        fileItem.status = InsertFileStatus::TRANSCODE_SUCCEEDED;  
    } else {
        fileItem.status = InsertFileStatus::TRANSCODE_FAILED;  
    }

    
    QString durationStr = recordJson["videoDuration"].toString();
    QStringList parts = durationStr.split(":");
    qint64 durationMs = 0;
    if (parts.size() == 3) {
        
        int hours = parts[0].toInt();
        int minutes = parts[1].toInt();
        int seconds = parts[2].toInt();
        durationMs = (hours * 3600 + minutes * 60 + seconds) * 1000;
    } else if (parts.size() == 2) {
        
        int minutes = parts[0].toInt();
        int seconds = parts[1].toInt();
        durationMs = (minutes * 60 + seconds) * 1000;
    }
    fileItem.durationMs = durationMs;
}

void ClientService::parseInsertVideolistJson(const QJsonObject& json, QList<InsertFileItem>& insertFileList, int& totalCount, const QString& sassUrl)
{
    totalCount = json["totalCount"].toInt();  
    QJsonArray records = json["records"].toArray();

    for (const auto& recordVal : records) {
        QJsonObject recordJson = recordVal.toObject();
        InsertFileItem fileItem;
        parseInsertFileJson(recordJson, fileItem, sassUrl);
        insertFileList.emplace_back(fileItem);
    }
}

bool ClientService::getInsertFile(const QString& sassUrl, const QString& userId, const QString& token, const QString& videoRoomId,
                                 const QString& roomInfoId, InsertFileItem& fileItem, QString& errMessage)
{
    errMessage.clear();

    
    QJsonObject reqData;
    reqData["videoRoomId"] = videoRoomId;
    reqData["roomInfoId"] = roomInfoId;

    QString url = QString("%1/livesaas/SelectVideoRoomById").arg(sassUrl);
    HttpClient* client = HttpClient::instance();
    
    
    struct curl_slist* headers = client->createHeaders();
    client->addHeader(&headers, "Authorization", makeAuthorizationHeader(token));
    client->addHeader(&headers, "Content-Type", "application/json");
    
    
    QString httpErrMsg;
    QJsonObject response = client->post(url, reqData, headers, httpErrMsg);
    client->freeHeaders(headers);

    
    if (!httpErrMsg.isEmpty()) {
        errMessage = httpErrMsg;
        LOG_WARNING(QString("获取插播视频详情HTTP请求失败: %1").arg(errMessage).toStdString());
        return false;
    }

    
    if (response["code"].toInt() != 200) {
        errMessage = response["msg"].toString();
        LOG_WARNING(QString("获取插播视频详情失败: %1").arg(errMessage).toStdString());
        return false;
    }

    
    QJsonObject dataJson = response["data"].toObject();
    parseInsertFileJson(dataJson, fileItem, sassUrl);
    return true;
}

bool ClientService::getStreamName(const QString& sassUrl, const QString& token, const QString& roomInfoId,
                                  StreamNameInfo& streamInfo, QString& errMessage)
{
    errMessage.clear();
    streamInfo = StreamNameInfo{};

    QJsonObject reqData;
    reqData["roomInfoId"] = roomInfoId;

    QString url = QString("%1/livesaas/StreamName").arg(sassUrl);
    HttpClient* client = HttpClient::instance();

    struct curl_slist* headers = client->createHeaders();
    client->addHeader(&headers, "Authorization", makeAuthorizationHeader(token));
    client->addHeader(&headers, "Content-Type", "application/json");

    QString httpErrMsg;
    QJsonObject response = client->post(url, reqData, headers, httpErrMsg);
    client->freeHeaders(headers);

    if (!httpErrMsg.isEmpty()) {
        errMessage = httpErrMsg;
        LOG_WARNING(QString("Get StreamName HTTP request failed: %1").arg(errMessage).toStdString());
        return false;
    }

    if (response["code"].toInt() != 200) {
        errMessage = response["msg"].toString();
        LOG_WARNING(QString("Get StreamName failed: %1").arg(errMessage).toStdString());
        return false;
    }

    QJsonObject dataJson = response["data"].toObject();
    streamInfo.isOpen = dataJson["isOpen"].toInt();
    streamInfo.livePartnerUrl = dataJson["livePartnerUrl"].toString();
    streamInfo.obsServer = dataJson["obsServer"].toString();
    streamInfo.obsStreamKey = dataJson["obsStreamKey"].toString();
    streamInfo.pushStreamId = dataJson["pushStreamId"].toString();
    streamInfo.streamName = dataJson["streamName"].toString();
    streamInfo.streamState = dataJson["streamState"].toInt();
    streamInfo.time = dataJson["time"].toString();

    const QJsonArray pullStreamArray = dataJson["pullStreamUrlList"].toArray();
    for (const auto& pullStreamValue : pullStreamArray) {
        if (pullStreamValue.isString()) {
            streamInfo.pullStreamUrlList.append(pullStreamValue.toString());
        }
    }

    LOG_INFO(QString("Get StreamName succeed - roomInfoId: %1, obsServer: %2, streamName: %3, streamState: %4")
             .arg(roomInfoId)
             .arg(streamInfo.obsServer)
             .arg(streamInfo.streamName)
             .arg(streamInfo.streamState)
             .toStdString());
    return true;
}
void ClientService::parseLiveListJson(const QJsonObject& json, QList<LiveItem>& liveList, int& totalCount)
{
    totalCount = json["totalCount"].toInt();  QJsonArray records = json["records"].toArray();

    for (const auto& recordVal : records) {
        QJsonObject recordJson = recordVal.toObject();
        LiveItem liveItem;

        
        liveItem.liveId =  recordJson["roomInfoId"].toString();/*recordJson["id"].toString();*/
        liveItem.roomNumber = recordJson["roomNumber"].toString();
        liveItem.title = recordJson["roomTitle"].toString();  
        liveItem.createTime = parseApiDateTime(recordJson["createTime"].toString());
        liveItem.startTime = parseApiDateTime(recordJson["liveStartTime"].toString());
        liveItem.endTime = parseApiDateTime(recordJson["liveEndTime"].toString());
        liveItem.roomType = recordJson["roomType"].toInt();
        liveItem.horizontalImageUrl = recordJson["horizontalImageUrl"].toString();
        liveItem.verticalImageUrl = recordJson["verticalImageUrl"].toString();
        liveItem.liveShareImgUrl = recordJson["liveShareImgUrl"].toString();

        
        if (recordJson.contains("pushStreamNameUrl") && recordJson["pushStreamNameUrl"].isString()) {
            liveItem.pushUrl.push_back(recordJson["pushStreamNameUrl"].toString());
        }

        
        int statusCode = recordJson["roomState"].toInt();
        liveItem.roomState = statusCode;
        if (statusCode == 1) liveItem.status = LiveStatus::PENDING;
        else if (statusCode == 2) liveItem.status = LiveStatus::LIVE;
        else if (statusCode == 3) liveItem.status = LiveStatus::ENDED;
        else liveItem.status = LiveStatus::PENDING;
        liveItem.reserveCount = recordJson["bookingNum"].toInt();           
        liveItem.viewCount = recordJson["viewerNum"].toInt();               

        liveItem.setVideoScreenMode(recordJson["videoScreenMode"].toInt(1));

        liveList.append(liveItem);
    }
}


void ClientService::parseInsertFile(const QJsonObject& recordJson, InsertFileItem& files)
{
    files.downloadUrl = recordJson["fileKey"].toString();
}

bool ClientService::genOnceLoginKey(const QString& baseUrl, const QString& userId, const QString& token, QString& loginKey, QString& errMessage)
{
    errMessage.clear();
    loginKey.clear();
    
    QJsonObject reqData;
    reqData["userId"] = userId;
    
    QString url = QString("%1/accounts/GenUserOnceLoginKey?userId=%2").arg(baseUrl).arg(userId);
    
    HttpClient* client = HttpClient::instance();
    
    
    struct curl_slist* headers = client->createHeaders();
    client->addHeader(&headers, "Authorization", makeAuthorizationHeader(token));
    client->addHeader(&headers, "Content-Type", "application/json");
    
    
    QString httpErrMsg;
    QJsonObject response = client->get(url, headers, httpErrMsg);
    client->freeHeaders(headers);

    
    if (!httpErrMsg.isEmpty()) {
        errMessage = httpErrMsg;
        LOG_WARNING(QString("Generate once login key HTTP request failed: %1").arg(errMessage).toStdString());
        return false;
    }

    
    if (response["code"].toInt() == 200) {
        
        loginKey = response["data"].toString();
        LOG_INFO(QString("Generate once login key succeed, key:%1").arg(loginKey).toStdString());
        return true;
    }
    
    errMessage = response["msg"].toString();
    LOG_WARNING(QString("Generate once login key failed:%1").arg(errMessage).toStdString());
    return false;
}
