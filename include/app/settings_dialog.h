#pragma once

#include <QDialog>
#include <vector>
#include <string>
#include "encoder/encoder_config.h"
#include "audio_engine/audio_engine.h"
#include "video_engine/video_engine.h"

namespace Ui {
class SettingsPanel;
}

namespace live_assistant {

/**
 * 设置面板标签页枚举
 */
enum class SettingsTab {
    Video = 0,      // 视频设置
    Audio = 1,      // 音频设置
    Camera = 2,     // 摄像头设置
    Background = 3  // 背景设置
};

/**
 * 整合设置面板对话框
 * 包含视频、音频、摄像头、背景四个设置页面
 */
class SettingsPanel : public QDialog {
    Q_OBJECT

public:
    explicit SettingsPanel(QWidget *parent = nullptr, SettingsTab defaultTab = SettingsTab::Video);
    ~SettingsPanel();

    // 设置默认标签页（打开时自动切换）
    void setDefaultTab(SettingsTab tab);
    
    // ===== 视频设置 =====
    void set_video_config(const VideoEncoderConfig& config);
    VideoEncoderConfig get_video_config() const;

    // ===== 音频设置 =====
    void set_audio_config(const AudioEncoderConfig& config);
    AudioEncoderConfig get_audio_config() const;
    void set_available_microphones(const std::vector<AudioEngine::AudioDeviceInfo>& mics, const std::string& current_mic_id);
    void set_available_speakers(const std::vector<AudioEngine::AudioDeviceInfo>& speakers, const std::string& current_speaker_id);
    std::string get_selected_microphone_id() const;
    std::string get_selected_speaker_id() const;
    float get_microphone_volume() const;
    float get_speaker_volume() const;

    // ===== 摄像头设置 =====
    void set_available_cameras(const std::vector<VideoEngine::CameraChoice>& cameras);
    void set_camera_config(const std::string& device_id, const std::string& resolution, int fps, bool mirror);
    std::string get_selected_camera_id() const;
    std::string get_camera_resolution() const;
    int get_camera_fps() const;
    bool is_camera_mirror() const;

    // ===== 背景设置 =====
    void set_background_type(const QString& type);
    void set_background_blur(int value);
    QString get_background_type() const;
    int get_background_blur() const;

private slots:
    void onMicVolumeChanged(int value);
    void onSpeakerVolumeChanged(int value);
    void onBgBlurChanged(int value);

private:
    void setupConnections();
    void updateNavButtons(SettingsTab tab);
    void showTab(SettingsTab tab);

    Ui::SettingsPanel *ui;
    
    // 配置存储
    VideoEncoderConfig video_config_;
    AudioEncoderConfig audio_config_;
    
    // 当前选中的标签页
    SettingsTab currentTab_ = SettingsTab::Video;
};

} // namespace live_assistant
