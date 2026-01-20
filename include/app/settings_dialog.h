#pragma once

#include <QDialog>
#include <vector>
#include <string>
#include "encoder/encoder_config.h"
#include "audio_engine/audio_engine.h"

namespace Ui {
class SettingsDialog;
}

namespace live_assistant {

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget *parent = nullptr);
    ~SettingsDialog();

    // 使用当前配置初始化对话框
    void set_current_video_config(const VideoEncoderConfig& config);
    void set_current_audio_config(const AudioEncoderConfig& config);
    void set_available_microphones(const std::vector<AudioEngine::AudioDeviceInfo>& mics, const std::string& current_mic_id);

    // 获取用户设置的新配置
    VideoEncoderConfig get_video_config() const;
    AudioEncoderConfig get_audio_config() const;
    std::string get_selected_microphone_id() const;

private:
    void populate_comboboxes();

    Ui::SettingsDialog *ui;

    VideoEncoderConfig current_video_config_;
    AudioEncoderConfig current_audio_config_;
};

} // namespace live_assistant
