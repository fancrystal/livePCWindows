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

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <ctime>

#pragma comment(lib, "dbghelp.lib")

// 生成 dump 文件名
std::string generate_dump_filename() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm;
    localtime_s(&local_tm, &time_t_now);

    // 创建 dmps 目录（如果不存在）
    const std::string dump_dir = "dmps";
    try {
        if (!std::filesystem::exists(dump_dir)) {
            std::filesystem::create_directory(dump_dir);
        }
    } catch (...) {
        // 如果创建失败，使用当前目录
    }

    std::ostringstream oss;
    oss << dump_dir << "/LiveAssistant_"
        << std::put_time(&local_tm, "%Y%m%d_%H%M%S")
        << ".dmp";

    return oss.str();
}

// 异常处理函数，生成 minidump
LONG WINAPI exception_handler(EXCEPTION_POINTERS* exception_pointers) {
    std::string dump_file = generate_dump_filename();

    HANDLE dump_file_handle = CreateFileA(
        dump_file.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (dump_file_handle != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION dump_info;
        dump_info.ExceptionPointers = exception_pointers;
        dump_info.ThreadId = GetCurrentThreadId();
        dump_info.ClientPointers = FALSE;

        BOOL success = MiniDumpWriteDump(
            GetCurrentProcess(),
            GetCurrentProcessId(),
            dump_file_handle,
            MiniDumpNormal,
            &dump_info,
            nullptr,
            nullptr
        );

        CloseHandle(dump_file_handle);

        if (success) {
            // 记录崩溃信息到日志
            std::ofstream log("applogs/crash_log.txt", std::ios::app);
            if (log.is_open()) {
                auto now = std::chrono::system_clock::now();
                auto time_t_now = std::chrono::system_clock::to_time_t(now);
                std::tm local_tm;
                localtime_s(&local_tm, &time_t_now);
                log << "[" << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S") << "] ";
                log << "Application crashed. Dump file: " << dump_file << std::endl;
                log.close();
            }
        }
    }

    // 返回 EXCEPTION_EXECUTE_HANDLER 表示异常已处理
    // 返回 EXCEPTION_CONTINUE_SEARCH 表示让其他处理器处理
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int main(int argc, char *argv[]) {
#ifdef _WIN32
    // 设置未处理异常过滤器，用于生成崩溃转储
    SetUnhandledExceptionFilter(exception_handler);
#endif

    // 启用高 DPI 缩放支持（Qt6 默认启用，但这里显式设置以确保兼容性）
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

    // 设置高 DPI 缩放策略为 PassThrough（不进行四舍五入，保持精确的缩放比例）
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication a(argc, argv);

    // 设置应用程序图标（任务栏和窗口图标）
    a.setWindowIcon(QIcon(":/images/logo.ico"));

    // Register custom types for cross-thread signal/slot connections
    qRegisterMetaType<live_assistant::CaptureFrame>("CaptureFrame");

    // 加载配置（包括日志级别）
    ConfigManager::instance().loadConfig();
    ConfigManager::instance().applyLogLevel();

    // 清理超过保留天数的旧日志文件
    live_assistant::Log::cleanup_old_logs();

    LOG_INFO("Starting LiveAssistant...");

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

                    // 先隐藏主窗口，保持响应
                    main_window->hide();

                    // 异步清理资源，不阻塞UI
                    QTimer::singleShot(50, main_window, [main_window]() {
                        LOG_INFO("Async cleanup started");
                        main_window->stop_all_capture_sources();
                        main_window->save_scenes_config();
                        LOG_INFO("Async cleanup finished");
                    });

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

    // Connect local stream success signal to show main window directly
    QObject::connect(&login_window, &live_assistant::LoginWindow::local_stream_success,
        [&login_window, &main_window](const QString& rtmpUrl) {
        LOG_INFO(QString("Local stream success - RTMP URL: %1").arg(rtmpUrl).toStdString());

        // 先关闭登录窗口
        login_window.close();

        // 创建主窗口
        if (!main_window) {
            main_window = new live_assistant::MainWindow();

            // 本地推流模式：退出时直接退出程序，不返回直播列表
            QObject::connect(main_window, &live_assistant::MainWindow::request_return_to_live_list,
                [main_window]() {
                LOG_INFO("Local stream mode - exiting application");

                // 先隐藏主窗口
                main_window->hide();

                // 异步清理资源
                QTimer::singleShot(50, main_window, [main_window]() {
                    LOG_INFO("Async cleanup started");
                    main_window->stop_all_capture_sources();
                    main_window->save_scenes_config();
                    LOG_INFO("Async cleanup finished");
                });

                // 延迟退出程序
                QTimer::singleShot(500, []() {
                    LOG_INFO("Quitting application from local stream mode");
                    QApplication::quit();
                });
            });
        }

        // 设置为本地推流模式
        main_window->set_local_stream_mode(true);

        // 设置推流地址
        main_window->set_rtmp_target(rtmpUrl, "");

        // 创建空的 LiveItem（本地推流不需要真实的直播间）
        LiveItem liveItem;
        liveItem.title = "本地推流";
        liveItem.liveId = "local_stream";

        // 设置直播间信息
        main_window->setLiveItem(liveItem);
        main_window->set_live_id("local_stream");

        // 先显示窗口（禁用更新），避免初始化过程中的闪烁
        main_window->show();
        main_window->setUpdatesEnabled(false);

        // 延迟启用窗口更新
        QTimer::singleShot(0, main_window, [main_window]() {
            main_window->setUpdatesEnabled(true);
            main_window->update();
            LOG_INFO("Window updates enabled after local stream initialization completed");
        });
    });

    // Show login window
    login_window.show();

    return a.exec();
}
