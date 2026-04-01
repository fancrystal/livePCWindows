#pragma once

#include <cstdint>

namespace live_assistant {

struct AppSettings;

// 设置分区 bitmask — 每一位代表一组相关配置
// 观察者通过 has_section() 判断自己关心的分区是否发生变化
enum class SettingsSection : uint32_t {
    None    = 0,
    Video   = 1 << 0,  // VideoEncoderConfig
    Audio   = 1 << 1,  // AudioEncoderConfig + 设备ID + 音量
    Canvas  = 1 << 2,  // CanvasConfig（分辨率/方向）
    Camera  = 1 << 3,  // 摄像头设备ID、分辨率、帧率、镜像
    UIState = 1 << 4,  // 退出偏好等UI状态
    All     = 0xFFFFFFFF
};

inline SettingsSection operator|(SettingsSection a, SettingsSection b) {
    return static_cast<SettingsSection>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline SettingsSection& operator|=(SettingsSection& a, SettingsSection b) {
    a = a | b;
    return a;
}

inline bool has_section(SettingsSection mask, SettingsSection bit) {
    return (static_cast<uint32_t>(mask) & static_cast<uint32_t>(bit)) != 0;
}

// 设置观察者接口
// 各子系统通过实现此接口，在配置变更时自动收到通知
class ISettingsObserver {
public:
    virtual ~ISettingsObserver() = default;

    // 当 AppSettings 发生变化时被调用
    // changed: 本次变更的分区 bitmask，观察者应检查后再做耗时操作
    virtual void on_settings_changed(const AppSettings& settings,
                                     SettingsSection changed) = 0;
};

} // namespace live_assistant
