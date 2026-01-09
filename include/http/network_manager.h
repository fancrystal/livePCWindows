#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <QObject>
#include <QThread>
#include <QWebSocket>
#include <QJsonObject>
#include <QMutex>

class NetworkManager : public QObject
{
    Q_OBJECT
public:
    explicit NetworkManager(QObject *parent = nullptr);
    ~NetworkManager();

    void setServerAddress(const QString &address);
    void setCredentials(const QString &userId, const QString &token);
    void setOnceKey(const QString& oncekey);
    void connectToServer();
    void disconnectFromServer();

    void sendChatMessage(const QString &userId, const QString &message);
    void sendUserStatus(const QString &userId, bool isMuted, bool isKicked);
    void sendMuteAllStatus(bool isMuteAll);
    void sendInsertVideoStatus(const QString& roomInfoId, const QString& fileId, int fileState, int percent = 0);

signals:
    // 接收到聊天消息
    void chatMessageReceived(const QString &userId, const QString &message);
    // 用户状态更新
    void userStatusUpdated(const QString &userId, bool isMuted, bool isKicked);
    // 全员禁言状态更新
    void muteAllStatusUpdated(bool isMuteAll);
    // 用户加入
    void userJoined(const QString &userId, const QString &userName);
    // 用户离开
    void userLeft(const QString &userId);
    // 连接状态变化
    void connectionStatusChanged(bool connected);
    // 错误信息
    void errorOccurred(const QString &error);
    
    // 服务端广播消息信号
    void insertVideoTranscoded(const QString &fileId, int fileState);
    void streamStopped(const QString &roomInfoId);
    void startInsertVideo(const QString &fileId);
    void stopInsertVideo(const QString &fileId);
    void videoStreamStateChanged(const QString &userId, const QString &streamName, int streamState, int errCode, const QString &errMsg);
    void insertVideoResult(const QString &fileId, bool result);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString &message);
    void onError(QAbstractSocket::SocketError error);

private:
    QThread m_workerThread;
    QWebSocket *m_webSocket;
    QString m_serverAddress; //ws://localhost:9085/ws
    QString m_userId;
    QString m_token;
    QMutex m_mutex;
    QString m_Oncekey;

    void sendMessage(const QJsonObject &message);
};

#endif // NETWORK_MANAGER_H
