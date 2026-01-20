#include "app/settings_dialog.h"
#include "ui_settings_dialog.h"

#include <QStringList>

namespace live_assistant {

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent),
      ui(new Ui::SettingsDialog) {
    ui->setupUi(this);

    populate_comboboxes();
}

SettingsDialog::~SettingsDialog() {
    delete ui;
}

void SettingsDialog::populate_comboboxes() {
    // Video
    ui->comboBox_resolution->clear();
    ui->comboBox_resolution->addItems({"1920x1080", "1280x720", "640x360"});

    ui->comboBox_fps->clear();
    ui->comboBox_fps->addItems({"15", "20", "25", "30", "60"});

    // Audio
    ui->comboBox_sampleRate->clear();
    ui->comboBox_sampleRate->addItems({"44100", "48000"});

    ui->comboBox_channels->clear();
    ui->comboBox_channels->addItems({"1", "2"});

    ui->comboBox_audioBitrate->clear();
    ui->comboBox_audioBitrate->addItems({"64", "96", "128", "160"});
}

void SettingsDialog::set_available_microphones(const std::vector<AudioEngine::AudioDeviceInfo>& mics, const std::string& current_mic_id) {
    ui->comboBox_microphone->clear();
    int current_index = -1;
    for (size_t i = 0; i < mics.size(); ++i) {
        // Display the UTF-8 name, store the stable ID as data
        ui->comboBox_microphone->addItem(QString::fromStdString(mics[i].name), QString::fromStdString(mics[i].id));
        if (mics[i].id == current_mic_id) {
            current_index = static_cast<int>(i);
        }
    }
    if (current_index != -1) {
        ui->comboBox_microphone->setCurrentIndex(current_index);
    }
}

void SettingsDialog::set_current_video_config(const VideoEncoderConfig& config) {
    current_video_config_ = config;

    const QString res = QString("%1x%2").arg(config.width).arg(config.height);
    int idx = ui->comboBox_resolution->findText(res);
    if (idx >= 0) ui->comboBox_resolution->setCurrentIndex(idx);

    idx = ui->comboBox_fps->findText(QString::number(config.fps));
    if (idx >= 0) ui->comboBox_fps->setCurrentIndex(idx);

    ui->spinBox_videoBitrate->setValue(config.bitrate / 1000); // kbps
    ui->spinBox_gop->setValue(config.gop);
}

void SettingsDialog::set_current_audio_config(const AudioEncoderConfig& config) {
    current_audio_config_ = config;

    int idx = ui->comboBox_sampleRate->findText(QString::number(config.sample_rate));
    if (idx >= 0) ui->comboBox_sampleRate->setCurrentIndex(idx);

    idx = ui->comboBox_channels->findText(QString::number(config.channels));
    if (idx >= 0) ui->comboBox_channels->setCurrentIndex(idx);

    idx = ui->comboBox_audioBitrate->findText(QString::number(config.bitrate / 1000));
    if (idx >= 0) ui->comboBox_audioBitrate->setCurrentIndex(idx);
}

VideoEncoderConfig SettingsDialog::get_video_config() const {
    VideoEncoderConfig cfg = current_video_config_;

    const QString res = ui->comboBox_resolution->currentText();
    const auto parts = res.split('x');
    if (parts.size() == 2) {
        cfg.width = parts[0].toInt();
        cfg.height = parts[1].toInt();
    }

    cfg.fps = ui->comboBox_fps->currentText().toInt();
    cfg.bitrate = ui->spinBox_videoBitrate->value() * 1000;
    cfg.gop = ui->spinBox_gop->value();

    return cfg;
}

AudioEncoderConfig SettingsDialog::get_audio_config() const {
    AudioEncoderConfig cfg = current_audio_config_;

    cfg.sample_rate = ui->comboBox_sampleRate->currentText().toInt();
    cfg.channels = ui->comboBox_channels->currentText().toInt();
    cfg.bitrate = ui->comboBox_audioBitrate->currentText().toInt() * 1000;

    return cfg;
}

std::string SettingsDialog::get_selected_microphone_id() const {
    // Return the stable ID stored in the item data
    return ui->comboBox_microphone->currentData().toString().toStdString();
}

} // namespace live_assistant
