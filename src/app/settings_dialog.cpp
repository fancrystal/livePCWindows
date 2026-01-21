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

    // Encoding mode (VBR/CBR/CQP)
    ui->comboBox_encodingMode->clear();
    ui->comboBox_encodingMode->addItems({"VBR", "CBR", "CQP"});

    // Max bitrate (kbps)
    ui->spinBox_maxBitrate->setMinimum(100);
    ui->spinBox_maxBitrate->setMaximum(100000);
    ui->spinBox_maxBitrate->setValue(2500);

    // We auto-select hardware encoder by priority; only expose prefer_hw toggle
    ui->checkBox_preferHW->setChecked(true);

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

    // Encoding mode
    QString modeStr = "VBR";
    if (config.mode == VideoEncodingMode::CBR) modeStr = "CBR";
    else if (config.mode == VideoEncodingMode::CQP) modeStr = "CQP";
    idx = ui->comboBox_encodingMode->findText(modeStr);
    if (idx >= 0) ui->comboBox_encodingMode->setCurrentIndex(idx);

    // max bitrate
    ui->spinBox_maxBitrate->setValue(config.max_bitrate / 1000);

    ui->checkBox_preferHW->setChecked(config.prefer_hw);
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

    // Encoding mode
    QString mode = ui->comboBox_encodingMode->currentText();
    if (mode == "CBR") cfg.mode = VideoEncodingMode::CBR;
    else if (mode == "CQP") cfg.mode = VideoEncodingMode::CQP;
    else cfg.mode = VideoEncodingMode::VBR;

    // max bitrate (kbps -> bps)
    cfg.max_bitrate = ui->spinBox_maxBitrate->value() * 1000;

    // We use internal priority to choose HW encoder (qsv>amf>nvenc). Do not expose specific hw selection.
    cfg.hw_accel = HWAccelerationType::NONE;
    cfg.prefer_hw = ui->checkBox_preferHW->isChecked();

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
