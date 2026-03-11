#include "customwebengineview.h"
#include <QWebEngineCookieStore>
#include <QNetworkCookie>
#include <QDebug>
#include <QScreen>
#include <QApplication>
#include <QColor>

class AuthRequestInterceptor : public QWebEngineUrlRequestInterceptor
{
public:
    explicit AuthRequestInterceptor(const QString& token, QObject* parent = nullptr)
        : QWebEngineUrlRequestInterceptor(parent), m_token(token)
    {}

    void interceptRequest(QWebEngineUrlRequestInfo& info) override
    {
        QString authorizationHeader = "Bearer " + m_token;
        info.setHttpHeader("Authorization", authorizationHeader.toUtf8());
    }

private:
    QString m_token;
};

CustomWebEnginePage::CustomWebEnginePage(QWebEngineProfile *profile, QObject *parent)
    : QWebEnginePage(profile, parent)
{
}

CustomWebEnginePage::CustomWebEnginePage(QObject *parent)
    : QWebEnginePage(parent)
{
}

void CustomWebEnginePage::javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level, const QString &message, int lineNumber, const QString &sourceID)
{
    Q_UNUSED(lineNumber);
    Q_UNUSED(sourceID);

    switch (level) {
    case InfoMessageLevel:
        qDebug() << "[WebEngine JS Info]" << message;
        break;
    case WarningMessageLevel:
        qDebug() << "[WebEngine JS Warning]" << message;
        break;
    case ErrorMessageLevel:
        qDebug() << "[WebEngine JS Error]" << message;
        break;
    }
}

CustomWebEngineView::CustomWebEngineView(QWidget *parent)
    : QWebEngineView(parent)
{
    initializeSettings();
}

void CustomWebEngineView::setCustomUrl(const QUrl &url)
{
    load(url);
}

void CustomWebEngineView::setAuthorizationToken(const QString &token, const QString &domain)
{
    // 创建interceptor
    AuthRequestInterceptor* interceptor = new AuthRequestInterceptor(token, this);
    page()->profile()->setUrlRequestInterceptor(interceptor);

    QWebEngineCookieStore* cookieStore = page()->profile()->cookieStore();
    QNetworkCookie cookie("token", token.toUtf8());
    cookie.setDomain(domain);
    cookie.setPath("/");
    cookieStore->setCookie(cookie);
}

void CustomWebEngineView::initializeSettings()
{
    CustomWebEnginePage* customPage = new CustomWebEnginePage(this);
    setPage(customPage);
    
    // 启用JavaScript
    settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    settings()->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, true);
    settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, true);
    
    // 启用插件
    settings()->setAttribute(QWebEngineSettings::PluginsEnabled, true);
    
    // 显示滚动条
    settings()->setAttribute(QWebEngineSettings::ShowScrollBars, true);
    
    // 字体设置
    settings()->setFontSize(QWebEngineSettings::DefaultFontSize, 14);
    settings()->setFontSize(QWebEngineSettings::DefaultFixedFontSize, 14);
    settings()->setFontSize(QWebEngineSettings::MinimumFontSize, 8);
    
    // 设置User-Agent
    page()->profile()->setHttpUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    
    // 设置黑色背景样式
    setStyleSheet(R"(
        QWebEngineView { 
            outline: none; 
            margin: 0px; 
            padding: 0px; 
            border: none; 
            background-color: #1e1e1e;
        }
    )");
    
    // 设置页面背景为黑色
    page()->setBackgroundColor(QColor("#1e1e1e"));
    
    // 布局设置
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(0, 0);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    
    // Native窗口属性（按照旧项目）
    setAttribute(Qt::WA_NativeWindow, false);
    setAttribute(Qt::WA_DontCreateNativeAncestors, true);
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    
    setZoomFactor(1.0);
    
    settings()->setAttribute(QWebEngineSettings::AutoLoadImages, true);
    
    settings()->setDefaultTextEncoding("UTF-8");
}
