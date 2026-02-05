// network_manager.cpp
#include "http/network_manager.h"
#include "common/log.h"
#include <QJsonDocument>
#include <QDebug>
#include <QDateTime>
#include <QNetworkRequest>

namespace live_assistant {

    // 静态成员变量初始化
    NetworkManager* NetworkManager::m_instance = nullptr;
    QMutex NetworkManager::m_mutex;

    // 获取单例实例（线程安全）
    NetworkManager* NetworkManager::instance()
    {
        if (!m_instance) {
            QMutexLocker locker(&m_mutex);  // 使用单例mutex
            if (!m_instance) {  // 双重检查
                m_instance = new NetworkManager();
            }
        }
        return m_instance;
    }

    NetworkManager::NetworkManager(QObject* parent)
        : QObject(parent), m_webSocket(nullptr)
    {
        // 创建WebSocket并移动到工作线程
        m_webSocket = new QWebSocket();
        m_webSocket->moveToThread(&m_workerThread);

        // 连接信号槽
        connect(&m_workerThread, &QThread::finished, m_webSocket, &QObject::deleteLater);
        connect(m_webSocket, &QWebSocket::connected, this, &NetworkManager::onConnected);
        connect(m_webSocket, &QWebSocket::disconnected, this, &NetworkManager::onDisconnected);
        connect(m_webSocket, &QWebSocket::textMessageReceived, this, &NetworkManager::onTextMessageReceived);
        connect(m_webSocket, &QWebSocket::errorOccurred, this, &NetworkManager::onError);

        // connect(m_webSocket, &QWebSocket::sslErrors, [&](const QList<QSslError>& errors) {
        //     // 忽略所有 SSL 错误（仅测试用）
        //     m_webSocket->ignoreSslErrors();
        // });

        // 启动工作线程
        m_workerThread.start();
    }

    NetworkManager::~NetworkManager()
    {
        disconnectFromServer();
        m_workerThread.quit();
        m_workerThread.wait();
    }

    void NetworkManager::setServerAddress(const QString& address)
    {
        QMutexLocker locker(&m_dataMutex);
        m_serverAddress = address;
    }

    void NetworkManager::setCredentials(const QString& userId, const QString& token)
    {
        QMutexLocker locker(&m_dataMutex);
        m_userId = userId;
        m_token = token;
    }
    void NetworkManager::setOnceKey(const QString& oncekey)
    {
        QMutexLocker locker(&m_dataMutex);
        m_Oncekey = oncekey;
    }
    void NetworkManager::connectToServer()
    {
        QMutexLocker locker(&m_dataMutex);
        if (m_webSocket && m_serverAddress.startsWith("wss")) {
            QNetworkRequest request(m_serverAddress);
            // 设置 Authorization 头（令牌）
            request.setRawHeader("Authorization", m_token.toUtf8());
            QMetaObject::invokeMethod(m_webSocket, "open", Qt::QueuedConnection, request);
        }
        else {
            emit errorOccurred("无效的服务器地址");
        }
    }

    void NetworkManager::disconnectFromServer()
    {
        if (m_webSocket) {
            QMetaObject::invokeMethod(m_webSocket, "close", Qt::QueuedConnection);
        }
    }

    void NetworkManager::sendChatMessage(const QString& userId, const QString& message)
    {
        QJsonObject msg;
        msg["type"] = "chat";
        msg["userId"] = userId;
        msg["message"] = message;
        msg["timestamp"] = QDateTime::currentMSecsSinceEpoch();

        sendMessage(msg);
    }

    void NetworkManager::sendUserStatus(const QString& userId, bool isMuted, bool isKicked)
    {
        QJsonObject msg;
        msg["type"] = "userStatus";
        msg["targetUserId"] = userId;
        msg["isMuted"] = isMuted;
        msg["isKicked"] = isKicked;
        msg["operatorUserId"] = m_userId;

        sendMessage(msg);
    }

    void NetworkManager::sendMuteAllStatus(bool isMuteAll)
    {
        QJsonObject msg;
        msg["type"] = "muteAll";
        msg["isMuteAll"] = isMuteAll;
        msg["operatorUserId"] = m_userId;

        sendMessage(msg);
    }

    void NetworkManager::sendInsertVideoStatus(const QString& roomInfoId, const QString& fileId, int fileState, int percent)
    {
        QJsonObject msg;
        QJsonObject data;
        data["fileId"] = fileId;
        data["fileState"] = fileState;
        data["percent"] = percent;
        msg["data"] = data;
        msg["messageDesc"] = "插播视频状态";
        msg["messageType"] = 10009;
        msg["roominfoId"] = roomInfoId;

        sendMessage(msg);
    }

    void NetworkManager::sendMessage(const QJsonObject& message)
    {
        if (!m_webSocket || m_webSocket->state() != QAbstractSocket::ConnectedState) {
            emit errorOccurred("未连接到服务器");
            return;
        }

        QJsonDocument doc(message);
        QString jsonString = doc.toJson(QJsonDocument::Compact);

        QMetaObject::invokeMethod(m_webSocket, "sendTextMessage", Qt::QueuedConnection,
            Q_ARG(QString, jsonString));
    }

    void NetworkManager::onConnected()
    {
        live_assistant::Log::info("Connected to server");
        emit connectionStatusChanged(true);

        // 发送认证信息
        QJsonObject authMsg;
        authMsg["type"] = "auth";
        authMsg["userId"] = m_userId;
        authMsg["token"] = m_token;

        sendMessage(authMsg);
    }

    void NetworkManager::onDisconnected()
    {
        live_assistant::Log::info("Disconnected from server");
        emit connectionStatusChanged(false);
    }

    void NetworkManager::onTextMessageReceived(const QString& message)
    {
        QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
        if (doc.isNull()) {
            emit errorOccurred("收到无效的JSON消息");
            return;
        }

        QJsonObject obj = doc.object();

        // 检查是否包含type字段（旧格式消息）
        if (obj.contains("type")) {
            QString type = obj["type"].toString();

            if (type == "chat") {
                QString userId = obj["userId"].toString();
                QString msg = obj["message"].toString();
                emit chatMessageReceived(userId, msg);
            }
            else if (type == "userStatus") {
                QString userId = obj["userId"].toString();
                bool isMuted = obj["isMuted"].toBool();
                bool isKicked = obj["isKicked"].toBool();
                emit userStatusUpdated(userId, isMuted, isKicked);
            }
            else if (type == "muteAll") {
                bool isMuteAll = obj["isMuteAll"].toBool();
                emit muteAllStatusUpdated(isMuteAll);
            }
            else if (type == "userJoined") {
                QString userId = obj["userId"].toString();
                QString userName = obj["userName"].toString();
                emit userJoined(userId, userName);
            }
            else if (type == "userLeft") {
                QString userId = obj["userId"].toString();
                emit userLeft(userId);
            }
        }
        // 检查是否包含messageType字段（新格式消息）
        else if (obj.contains("messageType")) {
            int messageType = obj["messageType"].toInt();

            switch (messageType) {
            case 10001: {
                // 插播视频转码完成通知
                QJsonObject data = obj["data"].toObject();
                QString fileId = data["fileId"].toString();
                int fileState = data["fileState"].toInt();
                emit insertVideoTranscoded(fileId, fileState);
                break;
            }
            case 10002: {
                // 断流通知
                QString roomInfoId = obj["roominfoId"].toString();
                emit streamStopped(roomInfoId);
                break;
            }
            case 10004: {
                // 开始执行插播视频
                QJsonObject data = obj["data"].toObject();
                QString fileId = data["fileId"].toString();
                emit startInsertVideo(fileId);
                break;
            }
            case 10006: {
                // 停止插播视频
                QJsonObject data = obj["data"].toObject();
                QString fileId = data["fileId"].toString();
                emit stopInsertVideo(fileId);
                break;
            }
            case 10007: {
                // 流状态通知
                QJsonObject data = obj["data"].toObject();
                QString userId = data["userId"].toString();
                QString streamName = data["streamName"].toString();
                int streamState = data["streamState"].toInt();
                int errCode = data["errCode"].toInt();
                QString errMsg = data["errMsg"].toString();
                emit videoStreamStateChanged(userId, streamName, streamState, errCode, errMsg);
                break;
            }
            case 20004: {
                // 插播视频结果-处理10004请求
                QJsonObject data = obj["data"].toObject();
                QString fileId = data["fileId"].toString();
                bool result = data["result"].toBool();
                emit insertVideoResult(fileId, result);
                break;
            }
            case 20006: {
                // 插播视频结果-处理10006请求
                QJsonObject data = obj["data"].toObject();
                QString fileId = data["fileId"].toString();
                bool result = data["result"].toBool();
                emit insertVideoResult(fileId, result);
                break;
            }
            default:
                // 未知消息类型，记录日志
                live_assistant::Log::info(QString("收到未知消息类型: %1").arg(messageType).toStdString());
                break;
            }
        }
    }

    void NetworkManager::onError(QAbstractSocket::SocketError error)
    {
        QString errorString = m_webSocket->errorString();
        emit errorOccurred(errorString);
    }
}