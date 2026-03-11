#ifndef CUSTOMWEBENGINEVIEW_H
#define CUSTOMWEBENGINEVIEW_H

#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QResizeEvent>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineCookieStore>
#include <QNetworkCookie>
#include <QWebEngineProfile>

class CustomWebEnginePage : public QWebEnginePage
{
    Q_OBJECT

public:
    explicit CustomWebEnginePage(QWebEngineProfile *profile, QObject *parent = nullptr);
    explicit CustomWebEnginePage(QObject *parent = nullptr);

protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level, const QString &message, int lineNumber, const QString &sourceID) override;
};

class CustomWebEngineView : public QWebEngineView
{
    Q_OBJECT

public:
    explicit CustomWebEngineView(QWidget *parent = nullptr);
    void setCustomUrl(const QUrl &url);
    void setAuthorizationToken(const QString &token, const QString &domain);

private:
    void initializeSettings();
};

#endif // CUSTOMWEBENGINEVIEW_H
