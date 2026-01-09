#include <QApplication>
#include "app/login_window.h"
#include "app/main_window.h"
#include "app/live_list_window.h"
#include "common/log.h"

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);
    
    // Initialize logging (enable DEBUG to collect detailed logs for capture debugging)
    live_assistant::Log::set_level(live_assistant::LogLevel::DEBUG);
    LOG_INFO("Starting LiveAssistant (DEBUG logs enabled)...");
    
    // Create login window
    live_assistant::LoginWindow login_window;
    
    // Create live list window (hidden initially)
    live_assistant::LiveListWindow live_list_window;
    
    // Create main window (hidden initially)
    live_assistant::MainWindow main_window;
    
    // Connect login success signal to show live list window
    QObject::connect(&login_window, &live_assistant::LoginWindow::login_success, &live_list_window, [&live_list_window, &login_window]() {
        // 设置用户信息
        live_list_window.set_user_info(login_window.user_id(), login_window.token());
        // 连接到服务器
        live_list_window.connect_to_server();
        // 显示直播列表窗口
        live_list_window.show();
    });
    
    // Connect live selected signal to show main window
    QObject::connect(&live_list_window, &live_assistant::LiveListWindow::live_selected, &main_window, [&main_window](const QString& live_id) {
        // 设置选中的直播ID
        main_window.set_live_id(live_id);
        // 显示主窗口
        main_window.show();
    });
    
    // Show login window
    login_window.show();
    
    return a.exec();
}
