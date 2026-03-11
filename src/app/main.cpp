#include <QApplication>
#include <QIcon>
#include <QTimer>
#include "app/login_window.h"
#include "app/live_list_window.h"
#include "app/main_window.h"
#include "http/live_item.h"
#include "app/config.h"
#include "common/log.h"
#include "scene_manager/icapture_source.h"
#include <qmetatype.h>

int main(int argc, char *argv[]) {
    // 启用高 DPI 缩放支持（Qt6 默认启用，但这里显式设置以确保兼容性）
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

    // 设置高 DPI 缩放策略为 PassThrough（不进行四舍五入，保持精确的缩放比例）
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication a(argc, argv);

    // 设置应用程序图标（任务栏和窗口图标）
    a.setWindowIcon(QIcon(":/images/logo.png"));

    // Register custom types for cross-thread signal/slot connections
    qRegisterMetaType<live_assistant::CaptureFrame>("CaptureFrame");

    // Initialize logging (enable DEBUG to collect detailed logs for capture debugging)
    live_assistant::Log::set_level(live_assistant::LogLevel::DEBUG);
    LOG_INFO("Starting LiveAssistant (DEBUG logs enabled)...");

    // Create login window
    live_assistant::LoginWindow login_window;

    // Create live list window (will be shown after login)
    live_assistant::LiveListWindow* live_list_window = nullptr;
    live_assistant::MainWindow* main_window = nullptr;

    // Connect login success signal to show live list window
    QObject::connect(&login_window, &live_assistant::LoginWindow::login_success, &login_window, [&live_list_window, &main_window, &login_window]() {
        LOG_INFO("Login success - creating live list window");

        // 先关闭登录窗口
        login_window.close();

        // 创建直播列表窗口，传入登录信息
        live_list_window = new live_assistant::LiveListWindow(login_window.user_id(), login_window.token());

        // 连接直播间选中信号到主窗口
        QObject::connect(live_list_window, &live_assistant::LiveListWindow::live_selected,
            [&live_list_window, &main_window, &login_window](const QString& live_id, const LiveItem& liveItem) {
            LOG_INFO(QString("Live selected: %1, title: %2").arg(live_id).arg(liveItem.title).toStdString());

            // 隐藏直播列表窗口
            live_list_window->hide();

            // 创建或获取主窗口
            if (!main_window) {
                main_window = new live_assistant::MainWindow();

                // 连接返回直播列表信号
                QObject::connect(main_window, &live_assistant::MainWindow::request_return_to_live_list,
                    [main_window, &live_list_window]() {
                    LOG_INFO("User requested to return to live list");

                    // 隐藏主窗口
                    main_window->hide();

                    // 显示直播列表窗口
                    if (live_list_window) {
                        live_list_window->show();
                        live_list_window->activateWindow();
                    }
                });
            }

            // 设置凭证信息
            QSettings settings("LiveClient", "Config");
            ServerConfig serverConfig;

            // 读取环境配置
            if (!settings.contains("server/env")) {
                settings.setValue("server/env", 0);  // 主动创建键并写入默认值
            }
            int envValue = settings.value("server/env", 0).toInt();
            // 根据环境加载默认配置
            switch (static_cast<ServerEnv>(envValue)) {
            case ServerEnv::Testing:
                serverConfig = DEFAULT_TEST_CONFIG;
                break;
            case ServerEnv::Development:
                serverConfig = DEFAULT_DEV_CONFIG;
                break;
            case ServerEnv::Production:
                serverConfig = DEFAULT_PROD_CONFIG;
                break;
            case ServerEnv::Local:
                serverConfig = DEFAULT_LOCAL_CONFIG;
                break;
            default:
                serverConfig = DEFAULT_PROD_CONFIG;
                break;
            }

            // 读取自定义配置
            serverConfig.loginUrl = settings.value("server/loginUrl", serverConfig.loginUrl).toString();
            serverConfig.liveUrl = settings.value("server/liveUrl", serverConfig.liveUrl).toString();
            serverConfig.socketUrl = settings.value("server/socketUrl", serverConfig.socketUrl).toString();
            serverConfig.encryptionKey = settings.value("server/encryptionKey", serverConfig.encryptionKey).toString();
            // TODO: 从配置中获取 socketUrl，这里暂时使用默认值
            main_window->setCredentials(serverConfig.socketUrl, login_window.user_id(), login_window.token(), serverConfig.liveUrl, login_window.getLoginKey());

            // 设置直播项信息
            main_window->setLiveItem(liveItem);

            // 先显示窗口（禁用更新），避免初始化过程中的闪烁
            main_window->show();
            main_window->setUpdatesEnabled(false);

            // 设置直播ID（会触发所有模块初始化）
            main_window->set_live_id(live_id);

            // 延迟启用窗口更新，等所有初始化完成后再显示
            QTimer::singleShot(0, main_window, [main_window]() {
                main_window->setUpdatesEnabled(true);
                main_window->update();
                LOG_INFO("Window updates enabled after all initialization completed");
            });
        });

        // 显示直播列表窗口
        live_list_window->show();
        // 登录窗口会自动关闭
    });

    // Show login window
    login_window.show();

    return a.exec();
}
