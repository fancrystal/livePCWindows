#include "app/app_settings.h"
#include "common/log.h"

#include <QSettings>
#include <QString>

namespace live_assistant {

// ============================================================
// 持久化
// ============================================================

void AppSettings::load() {
    QSettings s("LiveAssistant", "Settings");

    // --- 视频编码 ---
    video.prefer_hw = s.value("video/preferHw", true).toBool();
    video.fps       = s.value("video/fps", 30).toInt();
    video.bitrate   = s.value("video/bitrate", 2500000).toInt();
    // 分辨率由 canvas 决定，video.width/height 在 build_video_config_from_settings 中填充

    // --- 画布（分辨率/方向） ---
    const QString orientation = s.value("canvas/orientation", "landscape").toString();
    if (orientation == "portrait") {
        canvas = CanvasConfig(CanvasConfig::DisplayMode::PORTRAIT_9_16);
    } else {
        canvas = CanvasConfig(CanvasConfig::DisplayMode::LANDSCAPE_16_9);
    }

    // --- 音频编码 ---
    audio.encoder_config.bitrate     = s.value("audio/bitrate", 128000).toInt();
    audio.encoder_config.sample_rate = s.value("audio/sampleRate", 48000).toInt();
    audio.encoder_config.channels    = s.value("audio/channels", 2).toInt();

    // --- 音频设备与音量 ---
    audio.microphone_device_id = s.value("audio/micDeviceId", "").toString().toStdString();
    audio.speaker_device_id    = s.value("audio/speakerDeviceId", "").toString().toStdString();
    audio.mic_volume           = s.value("audio/micVolume", 0.4f).toFloat();
    audio.speaker_volume       = s.value("audio/speakerVolume", 0.4f).toFloat();
    audio.mic_enabled          = s.value("audio/micEnabled", true).toBool();
    audio.speaker_enabled      = s.value("audio/speakerEnabled", true).toBool();

    // --- 摄像头 ---
    camera.device_id  = s.value("camera/deviceId", "").toString().toStdString();
    camera.resolution = s.value("camera/resolution", "1280x720").toString().toStdString();
    camera.fps        = s.value("camera/fps", 30).toInt();
    camera.mirror     = s.value("camera/mirror", false).toBool();

    // --- UI 状态 ---
    ui_state.exit_preference = s.value("ui/exitPreference", 0).toInt();

    LOG_INFO("[AppSettings] Loaded: canvas=" +
             std::to_string(canvas.get_width()) + "x" + std::to_string(canvas.get_height()) +
             " video fps=" + std::to_string(video.fps) +
             " bitrate=" + std::to_string(video.bitrate) +
             " prefer_hw=" + std::to_string(video.prefer_hw) +
             " mic_vol=" + std::to_string(audio.mic_volume) +
             " spk_vol=" + std::to_string(audio.speaker_volume));
}

void AppSettings::save(SettingsSection sections) const {
    QSettings s("LiveAssistant", "Settings");

    if (has_section(sections, SettingsSection::Video)) {
        s.setValue("video/preferHw", video.prefer_hw);
        s.setValue("video/fps",      video.fps);
        s.setValue("video/bitrate",  video.bitrate);
    }

    if (has_section(sections, SettingsSection::Canvas)) {
        const bool portrait = (canvas.get_height() > canvas.get_width());
        s.setValue("canvas/orientation", portrait ? "portrait" : "landscape");
    }

    if (has_section(sections, SettingsSection::Audio)) {
        s.setValue("audio/bitrate",      audio.encoder_config.bitrate);
        s.setValue("audio/sampleRate",   audio.encoder_config.sample_rate);
        s.setValue("audio/channels",     audio.encoder_config.channels);
        s.setValue("audio/micDeviceId",  QString::fromStdString(audio.microphone_device_id));
        s.setValue("audio/speakerDeviceId", QString::fromStdString(audio.speaker_device_id));
        s.setValue("audio/micVolume",    audio.mic_volume);
        s.setValue("audio/speakerVolume", audio.speaker_volume);
        s.setValue("audio/micEnabled",   audio.mic_enabled);
        s.setValue("audio/speakerEnabled", audio.speaker_enabled);
    }

    if (has_section(sections, SettingsSection::Camera)) {
        s.setValue("camera/deviceId",   QString::fromStdString(camera.device_id));
        s.setValue("camera/resolution", QString::fromStdString(camera.resolution));
        s.setValue("camera/fps",        camera.fps);
        s.setValue("camera/mirror",     camera.mirror);
    }

    if (has_section(sections, SettingsSection::UIState)) {
        s.setValue("ui/exitPreference", ui_state.exit_preference);
    }
}

// ============================================================
// 观察者管理
// ============================================================

void AppSettings::add_observer(ISettingsObserver* observer) {
    if (observer) {
        observers_.push_back(observer);
    }
}

void AppSettings::remove_observer(ISettingsObserver* observer) {
    observers_.erase(
        std::remove(observers_.begin(), observers_.end(), observer),
        observers_.end());
}

void AppSettings::notify(SettingsSection changed) {
    for (auto* obs : observers_) {
        obs->on_settings_changed(*this, changed);
    }
}

} // namespace live_assistant
