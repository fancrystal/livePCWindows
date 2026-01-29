#include "app/settings_dialog.h"
#include "ui_settings_panel.h"
#include "ui_settings_dialog.h"

#include <QStringList>
#include <QSlider>
#include <QLabel>

namespace live_assistant {

// ==================== SettingsPanel 实现 ====================

SettingsPanel::SettingsPanel(QWidget *parent, SettingsTab defaultTab)
    : QDialog(parent),
      ui(new Ui::SettingsPanel) {
    ui->setupUi(this);
    
    // 设置窗口属性
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setFixedSize(720, 520);
    
    // 初始化视频选项
    ui->comboBox_video->addItems({"1920x1080", "1280x720", "640x360"});
    ui->comboBox_fps->addItems({"15", "20", "25", "30", "60"});
    
    // 初始化背景类型
    ui->comboBox_bgType->addItems({"无", "绿幕", "图片", "视频"});
    
    // 初始化摄像头分辨率和帧率
    ui->comboBox_camResolution->addItems({"640x360", "1280x720", "1920x1080"});
    ui->comboBox_camFps->addItems({"15", "25", "30", "60"});
    
    // 连接导航按钮
    setupConnections();
    
    // 设置默认标签页
    setDefaultTab(defaultTab);
}

SettingsPanel::~SettingsPanel() {
    delete ui;
}

void SettingsPanel::setupConnections() {
    // 视频按钮
    connect(ui->pushButton_video, &QPushButton::clicked, this, [this]() {
        showTab(SettingsTab::Video);
    });
    
    // 音频按钮
    connect(ui->pushButton_audio, &QPushButton::clicked, this, [this]() {
        showTab(SettingsTab::Audio);
    });
    
    // 摄像头按钮
    connect(ui->pushButton_camera, &QPushButton::clicked, this, [this]() {
        showTab(SettingsTab::Camera);
    });
    
    // 背景按钮
    connect(ui->pushButton_background, &QPushButton::clicked, this, [this]() {
        showTab(SettingsTab::Background);
    });
    
    // 麦克风音量滑块
    connect(ui->slider_micVolume, &QSlider::valueChanged, this, &SettingsPanel::onMicVolumeChanged);
    
    // 扬声器音量滑块
    connect(ui->slider_speakerVolume, &QSlider::valueChanged, this, &SettingsPanel::onSpeakerVolumeChanged);
    
    // 背景模糊滑块
    connect(ui->slider_bgBlur, &QSlider::valueChanged, this, &SettingsPanel::onBgBlurChanged);
}

void SettingsPanel::updateNavButtons(SettingsTab tab) {
    // 重置所有按钮状态
    ui->pushButton_video->setChecked(false);
    ui->pushButton_audio->setChecked(false);
    ui->pushButton_camera->setChecked(false);
    ui->pushButton_background->setChecked(false);
    
    // 设置当前选中按钮状态
    switch (tab) {
    case SettingsTab::Video:
        ui->pushButton_video->setChecked(true);
        break;
    case SettingsTab::Audio:
        ui->pushButton_audio->setChecked(true);
        break;
    case SettingsTab::Camera:
        ui->pushButton_camera->setChecked(true);
        break;
    case SettingsTab::Background:
        ui->pushButton_background->setChecked(true);
        break;
    }
}

void SettingsPanel::showTab(SettingsTab tab) {
    currentTab_ = tab;
    
    // 更新按钮状态
    updateNavButtons(tab);
    
    // 隐藏所有页面
    ui->page_video->hide();
    ui->page_audio->hide();
    ui->page_camera->hide();
    ui->page_background->hide();
    
    // 显示当前页面
    switch (tab) {
    case SettingsTab::Video:
        ui->page_video->show();
        break;
    case SettingsTab::Audio:
        ui->page_audio->show();
        break;
    case SettingsTab::Camera:
        ui->page_camera->show();
        break;
    case SettingsTab::Background:
        ui->page_background->show();
        break;
    }
}

void SettingsPanel::setDefaultTab(SettingsTab tab) {
    showTab(tab);
}

// ===== 视频设置 =====

void SettingsPanel::set_video_config(const VideoEncoderConfig& config) {
    video_config_ = config;
    
    const QString res = QString("%1x%2").arg(config.width).arg(config.height);
    int idx = ui->comboBox_video->findText(res);
    if (idx >= 0) ui->comboBox_video->setCurrentIndex(idx);
    
    idx = ui->comboBox_fps->findText(QString::number(config.fps));
    if (idx >= 0) ui->comboBox_fps->setCurrentIndex(idx);
    
    ui->spinBox_bitrate->setValue(config.bitrate / 1000);
}

VideoEncoderConfig SettingsPanel::get_video_config() const {
    VideoEncoderConfig cfg = video_config_;
    
    const QString res = ui->comboBox_video->currentText();
    const auto parts = res.split('x');
    if (parts.size() == 2) {
        cfg.width = parts[0].toInt();
        cfg.height = parts[1].toInt();
    }
    
    cfg.fps = ui->comboBox_fps->currentText().toInt();
    cfg.bitrate = ui->spinBox_bitrate->value() * 1000;
    
    return cfg;
}

// ===== 音频设置 =====

void SettingsPanel::set_audio_config(const AudioEncoderConfig& config) {
    audio_config_ = config;
    
    // 设置麦克风音量
    if (audio_config_.mic_volume >= 0 && audio_config_.mic_volume <= 1.0f) {
        ui->slider_micVolume->setValue(static_cast<int>(audio_config_.mic_volume * 100));
        ui->label_micVolumeValue->setText(QString::number(static_cast<int>(audio_config_.mic_volume * 100)) + "%");
    }
    
    // 设置扬声器音量
    if (audio_config_.speaker_volume >= 0 && audio_config_.speaker_volume <= 1.0f) {
        ui->slider_speakerVolume->setValue(static_cast<int>(audio_config_.speaker_volume * 100));
        ui->label_speakerVolumeValue->setText(QString::number(static_cast<int>(audio_config_.speaker_volume * 100)) + "%");
    }
}

AudioEncoderConfig SettingsPanel::get_audio_config() const {
    AudioEncoderConfig cfg = audio_config_;
    
    cfg.mic_volume = ui->slider_micVolume->value() / 100.0f;
    cfg.speaker_volume = ui->slider_speakerVolume->value() / 100.0f;
    
    return cfg;
}

void SettingsPanel::set_available_microphones(const std::vector<AudioEngine::AudioDeviceInfo>& mics, const std::string& current_mic_id) {
    ui->comboBox_mic->clear();
    int current_index = -1;
    for (size_t i = 0; i < mics.size(); ++i) {
        ui->comboBox_mic->addItem(QString::fromStdString(mics[i].name), QString::fromStdString(mics[i].id));
        if (mics[i].id == current_mic_id) {
            current_index = static_cast<int>(i);
        }
    }
    if (current_index != -1) {
        ui->comboBox_mic->setCurrentIndex(current_index);
    }
}

void SettingsPanel::set_available_speakers(const std::vector<AudioEngine::AudioDeviceInfo>& speakers, const std::string& current_speaker_id) {
    ui->comboBox_speaker->clear();
    int current_index = -1;
    for (size_t i = 0; i < speakers.size(); ++i) {
        ui->comboBox_speaker->addItem(QString::fromStdString(speakers[i].name), QString::fromStdString(speakers[i].id));
        if (speakers[i].id == current_speaker_id) {
            current_index = static_cast<int>(i);
        }
    }
    if (current_index != -1) {
        ui->comboBox_speaker->setCurrentIndex(current_index);
    }
}

std::string SettingsPanel::get_selected_microphone_id() const {
    return ui->comboBox_mic->currentData().toString().toStdString();
}

std::string SettingsPanel::get_selected_speaker_id() const {
    return ui->comboBox_speaker->currentData().toString().toStdString();
}

float SettingsPanel::get_microphone_volume() const {
    return ui->slider_micVolume->value() / 100.0f;
}

float SettingsPanel::get_speaker_volume() const {
    return ui->slider_speakerVolume->value() / 100.0f;
}

void SettingsPanel::onMicVolumeChanged(int value) {
    ui->label_micVolumeValue->setText(QString::number(value) + "%");
}

void SettingsPanel::onSpeakerVolumeChanged(int value) {
    ui->label_speakerVolumeValue->setText(QString::number(value) + "%");
}

// ===== 摄像头设置 =====

void SettingsPanel::set_available_cameras(const std::vector<VideoEngine::CameraChoice>& cameras) {
    ui->comboBox_camera->clear();
    
    for (const auto& cam : cameras) {
        ui->comboBox_camera->addItem(QString::fromStdString(cam.display_name), QString::fromStdString(cam.dshow_name));
    }
    
    if (!cameras.empty()) {
        ui->comboBox_camera->setCurrentIndex(0);
    }
}

void SettingsPanel::set_camera_config(const std::string& device_id, const std::string& resolution, int fps, bool mirror) {
    // 设置摄像头
    for (int i = 0; i < ui->comboBox_camera->count(); ++i) {
        if (ui->comboBox_camera->itemData(i).toString().toStdString() == device_id) {
            ui->comboBox_camera->setCurrentIndex(i);
            break;
        }
    }
    
    // 设置分辨率
    int idx = ui->comboBox_camResolution->findText(QString::fromStdString(resolution));
    if (idx >= 0) ui->comboBox_camResolution->setCurrentIndex(idx);
    
    // 设置帧率
    idx = ui->comboBox_camFps->findText(QString::number(fps));
    if (idx >= 0) ui->comboBox_camFps->setCurrentIndex(idx);
    
    // 设置镜像
    ui->checkBox_mirror->setChecked(mirror);
}

std::string SettingsPanel::get_selected_camera_id() const {
    return ui->comboBox_camera->currentData().toString().toStdString();
}

std::string SettingsPanel::get_camera_resolution() const {
    return ui->comboBox_camResolution->currentText().toStdString();
}

int SettingsPanel::get_camera_fps() const {
    return ui->comboBox_camFps->currentText().toInt();
}

bool SettingsPanel::is_camera_mirror() const {
    return ui->checkBox_mirror->isChecked();
}

// ===== 背景设置 =====

void SettingsPanel::set_background_type(const QString& type) {
    int idx = ui->comboBox_bgType->findText(type);
    if (idx >= 0) ui->comboBox_bgType->setCurrentIndex(idx);
}

void SettingsPanel::set_background_blur(int value) {
    ui->slider_bgBlur->setValue(value);
    ui->label_bgBlurValue->setText(QString::number(value) + "%");
}

QString SettingsPanel::get_background_type() const {
    return ui->comboBox_bgType->currentText();
}

int SettingsPanel::get_background_blur() const {
    return ui->slider_bgBlur->value();
}

void SettingsPanel::onBgBlurChanged(int value) {
    ui->label_bgBlurValue->setText(QString::number(value) + "%");
}

} // namespace live_assistant
