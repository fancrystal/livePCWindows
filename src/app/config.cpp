#include "app/config.h"
#include "common/log.h"
#include <QSettings>

ConfigManager& ConfigManager::instance() {
    static ConfigManager instance;
    return instance;
}

void ConfigManager::loadConfig() {
    QSettings settings("LiveAssistant", "Config");

    // 读取环境配置
    if (!settings.contains("server/env")) {
        settings.setValue("server/env", 1);  // 默认生产环境
    }
    currentEnv_ = static_cast<ServerEnv>(settings.value("server/env").toInt());

    // 根据环境加载默认配置
    switch (currentEnv_) {
    case ServerEnv::Production:
        config_ = DEFAULT_PROD_CONFIG;
        break;
    case ServerEnv::Testing:
        config_ = DEFAULT_TEST_CONFIG;
        break;
    case ServerEnv::Development:
        config_ = DEFAULT_DEV_CONFIG;
        break;
    case ServerEnv::Local:
        config_ = DEFAULT_LOCAL_CONFIG;
        break;
    default:
        config_ = DEFAULT_PROD_CONFIG;
        break;
    }

    // 读取自定义配置（允许覆盖默认值）
    config_.loginUrl = settings.value("server/loginUrl", config_.loginUrl).toString();
    config_.liveUrl = settings.value("server/liveUrl", config_.liveUrl).toString();
    config_.socketUrl = settings.value("server/socketUrl", config_.socketUrl).toString();
    config_.encryptionKey = settings.value("server/encryptionKey", config_.encryptionKey).toString();

    LOG_INFO(QString("Loaded config for env: %1").arg(static_cast<int>(currentEnv_)).toStdString());
}

void ConfigManager::saveConfig() {
    QSettings settings("LiveAssistant", "Config");

    // 保存环境配置
    settings.setValue("server/env", static_cast<int>(currentEnv_));

    // 保存服务器配置
    settings.setValue("server/loginUrl", config_.loginUrl);
    settings.setValue("server/liveUrl", config_.liveUrl);
    settings.setValue("server/socketUrl", config_.socketUrl);
    settings.setValue("server/encryptionKey", config_.encryptionKey);

    LOG_INFO("Config saved");
}

void ConfigManager::setEnvironment(ServerEnv env) {
    currentEnv_ = env;

    // 根据环境加载对应默认配置
    switch (env) {
    case ServerEnv::Production:
        config_ = DEFAULT_PROD_CONFIG;
        break;
    case ServerEnv::Testing:
        config_ = DEFAULT_TEST_CONFIG;
        break;
    case ServerEnv::Development:
        config_ = DEFAULT_DEV_CONFIG;
        break;
    case ServerEnv::Local:
        config_ = DEFAULT_LOCAL_CONFIG;
        break;
    }

    saveConfig();
    LOG_INFO(QString("Switched to environment: %1").arg(static_cast<int>(env)).toStdString());
}
