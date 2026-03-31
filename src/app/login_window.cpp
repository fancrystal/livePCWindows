#include "app/login_window.h"
#include "common/log.h"
#include "app/config.h"
#include "app/login_service.h"
#include "app/login_worker.h"
#include "http/http_client.h"

#include <QApplication>
#include <QQuickItem>
#include <QQuickWidget>
#include <QQmlContext>
#include <QQmlError>
#include <QUrl>
#include <QMouseEvent>
#include <QTimer>
#include <QScreen>
#include <QDir>
#include <QStandardPaths>
#include <QSettings>
namespace live_assistant {

LoginWindow::LoginWindow(QWidget *parent) :
    QMainWindow(parent),
    settings_("LiveAssistant", "Login") {
    
    // 设置窗口属性
    setWindowTitle("启点点直播 - 登录");
    setWindowFlag(Qt::FramelessWindowHint, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMinimumSize(800, 500);
    resize(1024, 640);
    
    // 初始化CURL库
    HttpClient::globalInit();
    
    // 加载服务器配置
    ConfigManager::instance().loadConfig();
    
    // 获取登录URL和加密密钥
    login_url_ = ConfigManager::instance().getLoginUrl();
    api_key_ = ConfigManager::instance().getEncryptionKey();
    
    LOG_INFO("LoginWindow created, loginUrl: " + login_url_.toStdString());
    
    // 创建QML登录界面
    qmlWidget_ = new QQuickWidget(this);
    qmlWidget_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    qmlWidget_->setClearColor(Qt::transparent);
    qmlWidget_->rootContext()->setContextProperty("loginWindow", this);
    
    // 设置QML为中央部件
    setCentralWidget(qmlWidget_);
    
    // 加载QML登录界面
    qmlWidget_->setSource(QUrl("qrc:/qml/Login.qml"));
    
    if (qmlWidget_->status() == QQuickWidget::Error) {
        const QList<QQmlError> errs = qmlWidget_->errors();
        for (const QQmlError &e : errs) {
            LOG_WARNING(std::string("QML Error: ") + e.toString().toStdString());
        }
    } else {
        LOG_INFO("QML Login loaded successfully");
    }

    // DPI 适配：监听屏幕 DPI 变化
    connect(windowHandle(), &QWindow::screenChanged, this, [this](QScreen* screen) {
        if (screen) {
            LOG_INFO("LoginWindow: Screen changed, DPI: " + std::to_string(screen->logicalDotsPerInch()));
            this->updateGeometry();
        }
    });

    if (windowHandle() && windowHandle()->screen()) {
        connect(windowHandle()->screen(), &QScreen::logicalDotsPerInchChanged, this, [this](qreal dpi) {
            LOG_INFO("LoginWindow: DPI changed to: " + std::to_string(dpi));
            this->updateGeometry();
        });
    }
}

LoginWindow::~LoginWindow() {
    LOG_INFO("LoginWindow destroyed");
    cleanupLoginThread();
}

// ========== QML调用方法 ==========

void LoginWindow::qmlLogin(const QString& username, const QString& password, bool remember) {
    LOG_INFO("qmlLogin called: username=" + username.toStdString() + ", remember=" + (remember ? "true" : "false"));

    if (isLoggingIn_) {
        LOG_WARNING("Login already in progress, ignoring duplicate request");
        emit login_failed("登录正在进行中，请稍候");
        return;
    }

    // 注意：QML端已经做了格式验证，这里只做基本的参数检查
    if (username.isEmpty() || password.isEmpty()) {
        emit login_failed("参数错误");
        return;
    }

    isLoggingIn_ = true;
    emit login_status_changed("正在登录...");

    LOG_INFO("创建登录线程...");

    // 清理之前的登录线程
    cleanupLoginThread();

    // 创建后台线程执行登录
    loginThread_ = new QThread(this);
    LoginWorker* worker = new LoginWorker(login_url_, api_key_, username, password, remember);
    worker->moveToThread(loginThread_);

    // 连接信号槽 - 注意连接顺序，先处理结果再删除worker
    connect(loginThread_, &QThread::started, worker, &LoginWorker::startLogin);
    connect(worker, &LoginWorker::loginSuccess, this, &LoginWindow::onLoginSuccess);
    connect(worker, &LoginWorker::loginFailed, this, &LoginWindow::onLoginFailed);
    connect(loginThread_, &QThread::finished, worker, &QObject::deleteLater);  // 线程结束时删除worker
    connect(loginThread_, &QThread::finished, loginThread_, &QThread::deleteLater);
    connect(loginThread_, &QThread::finished, this, &LoginWindow::cleanupLoginThread);

    LOG_INFO("启动登录线程...");
    // 启动线程
    loginThread_->start();
}

void LoginWindow::onLoginSuccess(const QString& userId, const QString& token, const QString& loginKey) {
    LOG_INFO(QString("收到登录成功信号: userId=%1").arg(userId).toStdString());

    user_id_ = userId;
    token_ = token;
    login_key_ = loginKey;

    isLoggingIn_ = false;
    emit login_status_changed("登录成功！");
    emit login_success();

    // 延迟关闭
    QTimer::singleShot(500, this, [this]() {
        close();
    });
}

void LoginWindow::onLoginFailed(const QString& errorMessage) {
    LOG_INFO(QString("C++ onLoginFailed: %1").arg(errorMessage).toStdString());

    isLoggingIn_ = false;
    emit login_failed(errorMessage);
}

void LoginWindow::cleanupLoginThread() {
    if (loginThread_ && loginThread_->isRunning()) {
        LOG_INFO("Cleaning up login thread...");
        loginThread_->quit();
        if (!loginThread_->wait(3000)) {
            LOG_WARNING("Login thread did not finish gracefully, forcing termination");
            loginThread_->terminate();
            loginThread_->wait();
        } else {
            LOG_INFO("Login thread finished gracefully");
        }
    }
    loginThread_ = nullptr;
}

bool LoginWindow::qmlHasSavedCredentials() {
    return settings_.value("remember", false).toBool() && 
           !settings_.value("username").toString().isEmpty();
}

QString LoginWindow::qmlGetSavedUsername() {
    return settings_.value("username").toString();
}

QString LoginWindow::qmlGetSavedPassword() {
    return settings_.value("password").toString();
}

void LoginWindow::qmlStartLocalStream(const QString& rtmpUrl) {
    LOG_INFO("qmlStartLocalStream called: rtmpUrl=" + rtmpUrl.toStdString());

    if (rtmpUrl.isEmpty()) {
        emit login_failed("推流地址不能为空");
        return;
    }

    // 验证 RTMP 地址格式
    if (!rtmpUrl.startsWith("rtmp://") && !rtmpUrl.startsWith("rtmps://")) {
        emit login_failed("请输入有效的 RTMP 推流地址（以 rtmp:// 或 rtmps:// 开头）");
        return;
    }

    // 保存本地推流地址
    local_stream_url_ = rtmpUrl;
    is_local_stream_mode_ = true;

    emit local_stream_success(rtmpUrl);
}

QString LoginWindow::qmlClearCache() {
    LOG_INFO("qmlClearCache: start");
    int total = 0;

    // 辅助 lambda：删除目录下匹配的文件，返回删除数量
    auto removeFiles = [&](const QString& dirPath, const QStringList& filters) {
        QDir dir(dirPath);
        if (!dir.exists()) return;
        const auto files = dir.entryInfoList(filters, QDir::Files);
        for (const QFileInfo& fi : files) {
            if (QFile::remove(fi.absoluteFilePath())) {
                ++total;
            }
        }
        LOG_INFO("qmlClearCache: removed " + std::to_string(files.size()) +
                 " file(s) from " + dirPath.toStdString());
    };

    // 1. 清理 AppSettings（推流/编码/音频/摄像头等持久化配置）
    QSettings appSettings("LiveAssistant", "Settings");
    appSettings.clear();
    LOG_INFO("qmlClearCache: AppSettings cleared");

    // 2. 清理日志文件
    // 日志使用相对路径 "applogs/"，基准是进程工作目录（开发时为 build_vs2019/，发布时为 exe 目录）
    // 两个路径都尝试，确保开发和生产环境都能命中
    removeFiles(QDir::currentPath() + "/applogs",        {"*.log"});
    removeFiles(QApplication::applicationDirPath() + "/applogs", {"*.log"});

    // 3. 清理插播视频缓存（QStandardPaths::CacheLocation）
    QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    removeFiles(cacheRoot, {"*.mp4", "*.flv", "*.ts", "*.m3u8"});

    // 4. 清理场景配置（scenes_*.json），视频源布局存在这里
    QString localDataDir  = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QString appConfigDir  = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    removeFiles(localDataDir, {"scenes_*.json"});
    removeFiles(appConfigDir, {"scenes_*.json"});

    LOG_INFO("qmlClearCache: done, total files removed = " + std::to_string(total));
    return QString("清理完成，共删除 %1 个文件").arg(total);
}

void LoginWindow::qmlSetStatus(const QString& status) {
    emit login_status_changed(status);
}

void LoginWindow::qmlSetLoginFailed(const QString& errorMessage) {
    LOG_INFO(QString("qmlSetLoginFailed called: %1").arg(errorMessage).toStdString());
    emit login_failed(errorMessage);
}

// ========== 内部方法 ==========

void LoginWindow::save_login_info_credentials(const QString& username, const QString& password, bool remember) {
    settings_.setValue("username", username);
    if (remember) {
        settings_.setValue("password", password);
    } else {
        settings_.remove("password");
    }
    settings_.setValue("remember", remember);
    LOG_INFO("Saved login credentials: username=" + username.toStdString());
}

// ========== 窗口拖拽支持 ==========

void LoginWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        dragging_ = true;
        dragStartPos_ = event->globalPos() - frameGeometry().topLeft();
        event->accept();
    }
}

void LoginWindow::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_ && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPos() - dragStartPos_);
        event->accept();
    }
}

void LoginWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        dragging_ = false;
        event->accept();
    }
}

void LoginWindow::closeEvent(QCloseEvent* event) {
    LOG_INFO("LoginWindow closeEvent triggered - closing login window only");
    event->accept();
    // 注意：不要调用QApplication::quit()，让main函数中的login_success信号处理后续窗口
}

void LoginWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        LOG_INFO("LoginWindow ESC key pressed - quitting application");
        event->accept();
        QApplication::quit();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

} // namespace live_assistant
