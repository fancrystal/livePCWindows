#include "app/screen_share.h"
#include "ui_screen_share.h"
#include "common/log.h"
#include <QMessageBox>

namespace live_assistant {

ScreenShareDialog::ScreenShareDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::ScreenShareDialog)
{
    ui->setupUi(this);

    // 设置默认值
    int fps_index = ui->comboBox_fps->findText("30");
    if (fps_index >= 0) {
        ui->comboBox_fps->setCurrentIndex(fps_index);
    }
    
    int resolution_index = ui->comboBox_resolution->findText("原尺寸");
    if (resolution_index >= 0) {
        ui->comboBox_resolution->setCurrentIndex(resolution_index);
    }

    connect(ui->comboBox_type, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ScreenShareDialog::on_type_changed);

    connect(ui->pushButton_ok, &QPushButton::clicked, this, &ScreenShareDialog::on_ok_clicked);
    connect(ui->pushButton_cancel, &QPushButton::clicked, this, &ScreenShareDialog::on_cancel_clicked);
}

ScreenShareDialog::~ScreenShareDialog()
{
    delete ui;
}

void ScreenShareDialog::set_available_screens(const std::vector<std::string>& screens) {
    screens_ = screens;

    if (ui->comboBox_type->currentIndex() == 0) {
        ui->comboBox_target->clear();
        for (const auto& screen : screens) {
            ui->comboBox_target->addItem(QString::fromStdString(screen));
        }
    }
}

void ScreenShareDialog::set_available_windows(const std::vector<std::string>& windows) {
    windows_ = windows;

    if (ui->comboBox_type->currentIndex() == 1) {
        ui->comboBox_target->clear();
        for (const auto& window : windows) {
            ui->comboBox_target->addItem(QString::fromStdString(window));
        }
    }
}

QString ScreenShareDialog::get_selected_target() const {
    return ui->comboBox_target->currentText();
}

bool ScreenShareDialog::is_screen_mode() const {
    return ui->comboBox_type->currentIndex() == 0;
}

int ScreenShareDialog::get_fps() const {
    return ui->comboBox_fps->currentText().toInt();
}

QString ScreenShareDialog::get_resolution() const {
    return ui->comboBox_resolution->currentText();
}

bool ScreenShareDialog::capture_cursor() const {
    return ui->checkBox_cursor->isChecked();
}

bool ScreenShareDialog::capture_border() const {
    return ui->checkBox_border->isChecked();
}

void ScreenShareDialog::set_fps(int fps) {
    int index = ui->comboBox_fps->findText(QString::number(fps));
    if (index >= 0) {
        ui->comboBox_fps->setCurrentIndex(index);
    }
}

void ScreenShareDialog::set_resolution(const QString& resolution) {
    int index = ui->comboBox_resolution->findText(resolution);
    if (index >= 0) {
        ui->comboBox_resolution->setCurrentIndex(index);
    }
}

void ScreenShareDialog::on_type_changed(int index) {
    ui->comboBox_target->clear();

    if (index == 0) {
        for (const auto& screen : screens_) {
            ui->comboBox_target->addItem(QString::fromStdString(screen));
        }
    } else {
        for (const auto& window : windows_) {
            ui->comboBox_target->addItem(QString::fromStdString(window));
        }
    }
}

void ScreenShareDialog::on_ok_clicked() {
    if (ui->comboBox_target->count() == 0) {
        LOG_WARNING("No target selected");
        QMessageBox::warning(this, "警告", "请选择捕获目标");
        return;
    }

    accept();
}

void ScreenShareDialog::on_cancel_clicked() {
    reject();
}

} // namespace live_assistant
