#include "app/config.h"
#include "common/log.h"
#include <QByteArray>
#include <QSettings>

namespace {
QString bundledCompatibilityEncryptionKey()
{
    static constexpr unsigned char keyBytes[] = {
        0x68, 0x37, 0x6b, 0x50, 0x39, 0x78, 0x52, 0x32,
        0x76, 0x4c, 0x6d, 0x51, 0x77, 0x45, 0x35, 0x74
    };
    return QString::fromLatin1(reinterpret_cast<const char*>(keyBytes), sizeof(keyBytes));
}
}

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

    // 读取日志级别配置
    if (!settings.contains("log/level")) {
        settings.setValue("log/level", static_cast<int>(LogLevelConfig::Info));  // 默认INFO
    }
    logLevel_ = static_cast<LogLevelConfig>(settings.value("log/level").toInt());

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
    config_.encryptionKey = qEnvironmentVariable("LIVEASSISTANT_ENCRYPTION_KEY");
    if (config_.encryptionKey.isEmpty()) {
        config_.encryptionKey = settings.value("server/encryptionKey").toString();
    }
    if (config_.encryptionKey.isEmpty()) {
        config_.encryptionKey = bundledCompatibilityEncryptionKey();
        LOG_WARNING("API encryption key loaded from bundled compatibility fallback. Prefer LIVEASSISTANT_ENCRYPTION_KEY or provision server/encryptionKey outside source control.");
    }

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

void ConfigManager::setLogLevel(LogLevelConfig level) {
    using namespace live_assistant;
    logLevel_ = level;
    QSettings settings("LiveAssistant", "Config");
    settings.setValue("log/level", static_cast<int>(level));
    applyLogLevel();
    LOG_INFO(QString("Log level set to: %1").arg(static_cast<int>(level)).toStdString());
}

void ConfigManager::applyLogLevel() const {
    using namespace live_assistant;
    switch (logLevel_) {
    case LogLevelConfig::Debug:
        Log::set_level(LogLevel::DEBUG);
        break;
    case LogLevelConfig::Info:
        Log::set_level(LogLevel::INFO);
        break;
    case LogLevelConfig::Warn:
        Log::set_level(LogLevel::WARN);
        break;
    case LogLevelConfig::Error:
        Log::set_level(LogLevel::ERR);
        break;
    }
}
