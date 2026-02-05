#ifndef CONFIG_H
#define CONFIG_H

#include <QString>

// 服务器环境类型（0=生产，1=测试，2=开发，3=本地）
enum class ServerEnv {
    Production = 0,  // 生产环境（默认）
    Testing = 1,     // 测试环境
    Development = 2, // 开发环境
    Local = 3        // 本地环境
};

// 服务器配置结构体
struct ServerConfig {
    QString loginUrl;       // 用户中心地址
    QString liveUrl;        // 企业直播地址
    QString socketUrl;      // Socket地址
    QString encryptionKey;  // 加密密钥
};

// 各环境默认配置
const ServerConfig DEFAULT_PROD_CONFIG = {
    "https://account-api.lxi-tech.com",
    "https://livesaas-api.lxi-tech.com",
    "wss://im-api.lxi-tech.com",
    "h7kP9xR2vLmQwE5t"
};

const ServerConfig DEFAULT_TEST_CONFIG = {
    "https://qdd-test.lxi-tech.com:15815",
    "https://qdd-test.lxi-tech.com:15816",
    "wss://qdd-test.lxi-tech.com:15830",
    "h7kP9xR2vLmQwE5t"
};

const ServerConfig DEFAULT_DEV_CONFIG = {
    "https://qdd-dev.lxi-tech.com:15815",
    "https://qdd-dev.lxi-tech.com:15816",
    "wss://qdd-dev.lxi-tech.com:15830",
    "h7kP9xR2vLmQwE5t"
};

const ServerConfig DEFAULT_LOCAL_CONFIG = {
    "http://192.168.3.47:9082",
    "http://192.168.3.47:9084",
    "ws://192.168.3.47:9085",
    "h7kP9xR2vLmQwE5t"
};

class ConfigManager {
public:
    static ConfigManager& instance();

    // 加载配置
    void loadConfig();
    
    // 保存配置
    void saveConfig();

    // 获取当前配置
    ServerConfig getConfig() const { return config_; }

    // 获取当前环境
    ServerEnv getCurrentEnv() const { return currentEnv_; }

    // 设置环境并加载对应配置
    void setEnvironment(ServerEnv env);

    // 获取登录URL
    QString getLoginUrl() const { return config_.loginUrl; }

    // 获取直播URL
    QString getLiveUrl() const { return config_.liveUrl; }

    // 获取Socket URL
    QString getSocketUrl() const { return config_.socketUrl; }

    // 获取加密Key
    QString getEncryptionKey() const { return config_.encryptionKey; }

private:
    ConfigManager() = default;
    ~ConfigManager() = default;
    
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    ServerConfig config_;
    ServerEnv currentEnv_ = ServerEnv::Production;
};

#endif // CONFIG_H
