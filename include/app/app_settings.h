#pragma once

#include "encoder/encoder_config.h"
#include "common/config_manager.h"
#include "app/settings_observer.h"

#include <string>
#include <vector>

namespace live_assistant {

// 音频相关设置（编码参数 + 设备 + 音量）
struct AudioSettings {
    AudioEncoderConfig encoder_config;
    std::string microphone_device_id;
    std::string speaker_device_id;
    float mic_volume     = 0.4f;
    float speaker_volume = 0.4f;
    bool  mic_enabled    = true;
    bool  speaker_enabled = false;
};

// 摄像头相关设置
struct CameraSettings {
    std::string device_id;
    std::string resolution = "1280x720";
    int  fps    = 30;
    bool mirror = false;
};

// UI 状态设置
struct UIStateSettings {
    int exit_preference = 0;  // 0=询问 1=最小化 2=退出
};

// ============================================================
// AppSettings — 全局唯一配置数据源
// 启动时从 QSettings 加载，用户修改后通过 apply_changes() 通知观察者
// ============================================================
class AppSettings {
public:
    // 各分区配置（直接访问，无需 getter/setter 样板代码）
    VideoEncoderConfig  video;
    AudioSettings       audio;
    CanvasConfig        canvas  = CanvasConfig::get_default();
    CameraSettings      camera;
    UIStateSettings     ui_state;
    // 注意：StreamConfig（推流地址/密钥）属于运行时参数，由业务逻辑直接设置，不持久化

    // --- 持久化 ---

    // 从 QSettings("LiveAssistant", "Settings") 加载所有分区
    void load();

    // 将指定分区持久化到 QSettings
    void save(SettingsSection sections = SettingsSection::All) const;

    // --- 观察者管理 ---

    void add_observer(ISettingsObserver* observer);
    void remove_observer(ISettingsObserver* observer);

    // 通知所有观察者指定分区已发生变化
    // 通常由 MainWindow::applySettingsPanelChanges 在更新字段后调用
    void notify(SettingsSection changed);

private:
    std::vector<ISettingsObserver*> observers_;
};

} // namespace live_assistant
