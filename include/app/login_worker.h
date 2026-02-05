#ifndef LOGIN_WORKER_H
#define LOGIN_WORKER_H

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QSettings>

class LoginWorker : public QObject
{
    Q_OBJECT

public:
    explicit LoginWorker(const QString& loginUrl, const QString& apiKey,
                         const QString& username, const QString& password,
                         QObject* parent = nullptr);
    ~LoginWorker();

signals:
    void loginSuccess(const QString& userId, const QString& token, const QString& loginKey);
    void loginFailed(const QString& errorMessage);

public slots:
    void startLogin();

private:
    QString loginUrl_;
    QString apiKey_;
    QString username_;
    QString password_;
    QSettings settings_;
};

#endif // LOGIN_WORKER_H
