#include "app/camera_settings.h"
#include "ui_camera_settings.h"

namespace live_assistant {

CameraSettingsDialog::CameraSettingsDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::CameraSettingsDialog)
{
    ui->setupUi(this);
    
    // 设置默认值
    set_resolution("640x360");
    set_fps(30);
    set_pixel_format("PIXEL_FORMAT_YUY2");
    set_mirror(false);
    set_camera_enabled(true);
    set_corner_rounding(false);
    
    // 连接按钮点击事件
    connect(ui->pushButton_basic, &QPushButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->page_basic);
        // 重置所有按钮状态
        ui->pushButton_basic->setChecked(true);
        ui->pushButton_camera->setChecked(false);
        ui->pushButton_beauty->setChecked(false);
        ui->pushButton_body->setChecked(false);
        ui->pushButton_makeup->setChecked(false);
        ui->pushButton_filter->setChecked(false);
        ui->pushButton_effects->setChecked(false);
        ui->pushButton_lens->setChecked(false);
    });
    
    connect(ui->pushButton_camera, &QPushButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->page_camera);
        // 重置所有按钮状态
        ui->pushButton_basic->setChecked(false);
        ui->pushButton_camera->setChecked(true);
        ui->pushButton_beauty->setChecked(false);
        ui->pushButton_body->setChecked(false);
        ui->pushButton_makeup->setChecked(false);
        ui->pushButton_filter->setChecked(false);
        ui->pushButton_effects->setChecked(false);
        ui->pushButton_lens->setChecked(false);
    });
    
    connect(ui->pushButton_beauty, &QPushButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->page_beauty);
        // 重置所有按钮状态
        ui->pushButton_basic->setChecked(false);
        ui->pushButton_camera->setChecked(false);
        ui->pushButton_beauty->setChecked(true);
        ui->pushButton_body->setChecked(false);
        ui->pushButton_makeup->setChecked(false);
        ui->pushButton_filter->setChecked(false);
        ui->pushButton_effects->setChecked(false);
        ui->pushButton_lens->setChecked(false);
    });
    
    connect(ui->pushButton_body, &QPushButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->page_body);
        // 重置所有按钮状态
        ui->pushButton_basic->setChecked(false);
        ui->pushButton_camera->setChecked(false);
        ui->pushButton_beauty->setChecked(false);
        ui->pushButton_body->setChecked(true);
        ui->pushButton_makeup->setChecked(false);
        ui->pushButton_filter->setChecked(false);
        ui->pushButton_effects->setChecked(false);
        ui->pushButton_lens->setChecked(false);
    });
    
    connect(ui->pushButton_makeup, &QPushButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->page_makeup);
        // 重置所有按钮状态
        ui->pushButton_basic->setChecked(false);
        ui->pushButton_camera->setChecked(false);
        ui->pushButton_beauty->setChecked(false);
        ui->pushButton_body->setChecked(false);
        ui->pushButton_makeup->setChecked(true);
        ui->pushButton_filter->setChecked(false);
        ui->pushButton_effects->setChecked(false);
        ui->pushButton_lens->setChecked(false);
    });
    
    connect(ui->pushButton_filter, &QPushButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->page_filter);
        // 重置所有按钮状态
        ui->pushButton_basic->setChecked(false);
        ui->pushButton_camera->setChecked(false);
        ui->pushButton_beauty->setChecked(false);
        ui->pushButton_body->setChecked(false);
        ui->pushButton_makeup->setChecked(false);
        ui->pushButton_filter->setChecked(true);
        ui->pushButton_effects->setChecked(false);
        ui->pushButton_lens->setChecked(false);
    });
    
    connect(ui->pushButton_effects, &QPushButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->page_effects);
        // 重置所有按钮状态
        ui->pushButton_basic->setChecked(false);
        ui->pushButton_camera->setChecked(false);
        ui->pushButton_beauty->setChecked(false);
        ui->pushButton_body->setChecked(false);
        ui->pushButton_makeup->setChecked(false);
        ui->pushButton_filter->setChecked(false);
        ui->pushButton_effects->setChecked(true);
        ui->pushButton_lens->setChecked(false);
    });
    
    connect(ui->pushButton_lens, &QPushButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->page_lens);
        // 重置所有按钮状态
        ui->pushButton_basic->setChecked(false);
        ui->pushButton_camera->setChecked(false);
        ui->pushButton_beauty->setChecked(false);
        ui->pushButton_body->setChecked(false);
        ui->pushButton_makeup->setChecked(false);
        ui->pushButton_filter->setChecked(false);
        ui->pushButton_effects->setChecked(false);
        ui->pushButton_lens->setChecked(true);
    });
    
    // 设置默认选中基础设置页面
    ui->pushButton_basic->setChecked(true);
    ui->stackedWidget->setCurrentWidget(ui->page_basic);
}

void CameraSettingsDialog::set_available_cameras(const std::vector<VideoEngine::CameraChoice>& cameras) {
    ui->comboBox_camera->clear();
    
    for (const auto& cam : cameras) {
        ui->comboBox_camera->addItem(QString::fromStdString(cam.display_name), QString::fromStdString(cam.dshow_name));
    }
    
    if (!cameras.empty()) {
        ui->comboBox_camera->setCurrentIndex(0);
    }
}

CameraSettingsDialog::~CameraSettingsDialog()
{
    delete ui;
}

void CameraSettingsDialog::set_camera_name(const std::string& name)
{
    // 查找匹配的摄像头名称
    int index = ui->comboBox_camera->findText(QString::fromStdString(name));
    if (index >= 0) {
        ui->comboBox_camera->setCurrentIndex(index);
    }
}

void CameraSettingsDialog::set_resolution(const std::string& resolution)
{
    // 查找匹配的分辨率
    int index = ui->comboBox_resolution->findText(QString::fromStdString(resolution));
    if (index >= 0) {
        ui->comboBox_resolution->setCurrentIndex(index);
    }
}

void CameraSettingsDialog::set_fps(int fps)
{
    // 查找匹配的帧率
    QString fps_str = QString::number(fps);
    int index = ui->comboBox_fps->findText(fps_str);
    if (index >= 0) {
        ui->comboBox_fps->setCurrentIndex(index);
    }
}

void CameraSettingsDialog::set_pixel_format(const std::string& format)
{
    // 查找匹配的像素格式
    int index = ui->comboBox_format->findText(QString::fromStdString(format));
    if (index >= 0) {
        ui->comboBox_format->setCurrentIndex(index);
    }
}

void CameraSettingsDialog::set_mirror(bool mirror)
{
    ui->checkBox_mirror->setChecked(mirror);
}

void CameraSettingsDialog::set_camera_enabled(bool enabled)
{
    ui->checkBox_toggle->setChecked(enabled);
}

void CameraSettingsDialog::set_corner_rounding(bool enabled)
{
    ui->checkBox_corner->setChecked(enabled);
}

std::string CameraSettingsDialog::get_camera_name() const
{
    return ui->comboBox_camera->currentText().toStdString();
}

std::string CameraSettingsDialog::get_camera_device_id() const
{
    return ui->comboBox_camera->currentData().toString().toStdString();
}

std::string CameraSettingsDialog::get_resolution() const
{
    return ui->comboBox_resolution->currentText().toStdString();
}

int CameraSettingsDialog::get_fps() const
{
    return ui->comboBox_fps->currentText().toInt();
}

std::string CameraSettingsDialog::get_pixel_format() const
{
    return ui->comboBox_format->currentText().toStdString();
}

bool CameraSettingsDialog::is_mirror() const
{
    return ui->checkBox_mirror->isChecked();
}

bool CameraSettingsDialog::is_camera_enabled() const
{
    return ui->checkBox_toggle->isChecked();
}

bool CameraSettingsDialog::is_corner_rounding() const
{
    return ui->checkBox_corner->isChecked();
}

void CameraSettingsDialog::on_pushButton_ok_clicked()
{
    // 发送设置改变信号
    emit settings_changed();
    
    // 关闭对话框
    accept();
}

void CameraSettingsDialog::on_pushButton_cancel_clicked()
{
    // 关闭对话框
    reject();
}

} // namespace live_assistant