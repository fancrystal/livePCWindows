#include "app/camera_settings.h"
#include "ui_camera_settings.h"
#include <QCamera>
#include <QMediaCaptureSession>
#include <QVideoWidget>
#include <QVideoSink>
#include <QMediaDevices>
#include <QCameraDevice>
#include <QImage>
#include <QPainter>
#include <QEvent>
#include <QMouseEvent>
#include <QComboBox>
#include "common/log.h"

namespace live_assistant {

CameraSettingsDialog::CameraSettingsDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::CameraSettingsDialog),
    media_capture_session_(nullptr),
    camera_(nullptr),
    video_sink_(nullptr),
    current_mirror_(false),
    comboBoxClickCount_(0),
    isComboBoxOpen_(false)
{
    ui->setupUi(this);

    // 设置默认值
    set_resolution("640x360");
    set_fps(30);
    set_pixel_format("PIXEL_FORMAT_YUY2");
    set_mirror(false);

    // 安装事件过滤器到所有下拉框
    ui->comboBox_camera->installEventFilter(this);
    ui->comboBox_resolution->installEventFilter(this);
    ui->comboBox_fps->installEventFilter(this);
    ui->comboBox_format->installEventFilter(this);

    // 连接下拉框的打开/关闭信号
    connect(ui->comboBox_camera, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        LOG_INFO("[CameraSettingsDialog] comboBox_camera activated, index=" + std::to_string(index));
    });
    connect(ui->comboBox_camera, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        LOG_INFO("[CameraSettingsDialog] comboBox_camera text changed to: " + text.toStdString());
    });

    // 连接按钮点击事件
    connect(ui->pushButton_basic, &QPushButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->page_basic);
        ui->pushButton_basic->setChecked(true);
        ui->pushButton_beauty->setChecked(false);
        ui->pushButton_body->setChecked(false);
        ui->pushButton_makeup->setChecked(false);
        ui->pushButton_filter->setChecked(false);
        ui->pushButton_effects->setChecked(false);
        ui->pushButton_lens->setChecked(false);
    });

    connect(ui->pushButton_beauty, &QPushButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->page_beauty);
        ui->pushButton_basic->setChecked(false);
        ui->pushButton_beauty->setChecked(true);
        ui->pushButton_body->setChecked(false);
        ui->pushButton_makeup->setChecked(false);
        ui->pushButton_filter->setChecked(false);
        ui->pushButton_effects->setChecked(false);
        ui->pushButton_lens->setChecked(false);
    });

    connect(ui->pushButton_body, &QPushButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->page_body);
        ui->pushButton_basic->setChecked(false);
        ui->pushButton_beauty->setChecked(false);
        ui->pushButton_body->setChecked(true);
        ui->pushButton_makeup->setChecked(false);
        ui->pushButton_filter->setChecked(false);
        ui->pushButton_effects->setChecked(false);
        ui->pushButton_lens->setChecked(false);
    });

    connect(ui->pushButton_makeup, &QPushButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->page_makeup);
        ui->pushButton_basic->setChecked(false);
        ui->pushButton_beauty->setChecked(false);
        ui->pushButton_body->setChecked(false);
        ui->pushButton_makeup->setChecked(true);
        ui->pushButton_filter->setChecked(false);
        ui->pushButton_effects->setChecked(false);
        ui->pushButton_lens->setChecked(false);
    });

    connect(ui->pushButton_filter, &QPushButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->page_filter);
        ui->pushButton_basic->setChecked(false);
        ui->pushButton_beauty->setChecked(false);
        ui->pushButton_body->setChecked(false);
        ui->pushButton_makeup->setChecked(false);
        ui->pushButton_filter->setChecked(true);
        ui->pushButton_effects->setChecked(false);
        ui->pushButton_lens->setChecked(false);
    });

    connect(ui->pushButton_effects, &QPushButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->page_effects);
        ui->pushButton_basic->setChecked(false);
        ui->pushButton_beauty->setChecked(false);
        ui->pushButton_body->setChecked(false);
        ui->pushButton_makeup->setChecked(false);
        ui->pushButton_filter->setChecked(false);
        ui->pushButton_effects->setChecked(true);
        ui->pushButton_lens->setChecked(false);
    });

    connect(ui->pushButton_lens, &QPushButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->page_lens);
        ui->pushButton_basic->setChecked(false);
        ui->pushButton_beauty->setChecked(false);
        ui->pushButton_body->setChecked(false);
        ui->pushButton_makeup->setChecked(false);
        ui->pushButton_filter->setChecked(false);
        ui->pushButton_effects->setChecked(false);
        ui->pushButton_lens->setChecked(true);
    });

    // 连接镜像复选框状态变化
    connect(ui->checkBox_mirror, &QCheckBox::stateChanged, this, [this](int state) {
        bool mirrored = (state == Qt::Checked);
        emit mirror_changed(mirrored);
        update_preview_mirror(mirrored);
    });

    // 连接摄像头选择变化
    connect(ui->comboBox_camera, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index >= 0) {
            restart_camera_preview();
        }
    });

    // 设置默认选中基础设置页面
    ui->pushButton_basic->setChecked(true);
    ui->stackedWidget->setCurrentWidget(ui->page_basic);
}

void CameraSettingsDialog::set_available_cameras(const std::vector<std::pair<std::string, std::string>>& cameras) {
    ui->comboBox_camera->clear();

    for (const auto& cam : cameras) {
        ui->comboBox_camera->addItem(QString::fromStdString(cam.first), QString::fromStdString(cam.second));
    }

    if (!cameras.empty()) {
        ui->comboBox_camera->setCurrentIndex(0);
    }
}

CameraSettingsDialog::~CameraSettingsDialog()
{
    // 停止摄像头预览
    stop_camera_preview();

    delete ui;
}

void CameraSettingsDialog::stop_camera_preview()
{
    if (camera_) {
        camera_->stop();
        delete camera_;
        camera_ = nullptr;
    }

    if (media_capture_session_) {
        delete media_capture_session_;
        media_capture_session_ = nullptr;
    }

    if (video_sink_) {
        delete video_sink_;
        video_sink_ = nullptr;
    }
}

void CameraSettingsDialog::restart_camera_preview()
{
    // 停止当前预览
    stop_camera_preview();

    // 获取选中的摄像头设备
    int index = ui->comboBox_camera->currentIndex();
    if (index < 0 || index >= ui->comboBox_camera->count()) {
        return;
    }

    // 获取摄像头设备信息
    QString camera_name = ui->comboBox_camera->itemText(index);

    // 查找对应的 QCameraDevice
    const QList<QCameraDevice> cameras = QMediaDevices::videoInputs();
    QCameraDevice selected_camera;
    bool found = false;

    for (const QCameraDevice& cam : cameras) {
        if (cam.description() == camera_name) {
            selected_camera = cam;
            found = true;
            break;
        }
    }

    if (!found) {
        return;
    }

    // 创建视频接收器
    video_sink_ = new QVideoSink(this);
    connect(video_sink_, &QVideoSink::videoFrameChanged, this, &CameraSettingsDialog::onVideoFrameChanged);

    // 创建摄像头对象
    camera_ = new QCamera(selected_camera, this);

    // 创建媒体捕获会话
    media_capture_session_ = new QMediaCaptureSession(this);
    media_capture_session_->setCamera(camera_);
    media_capture_session_->setVideoSink(video_sink_);

    // 启动摄像头
    camera_->start();
}

void CameraSettingsDialog::onVideoFrameChanged(const QVideoFrame& frame)
{
    if (!frame.isValid()) {
        return;
    }

    // 将视频帧转换为 QImage
    QVideoFrame cloneFrame(frame);
    if (!cloneFrame.map(QVideoFrame::ReadOnly)) {
        return;
    }

    // 使用 toImage 方法直接转换为 QImage
    QImage image = cloneFrame.toImage();
    cloneFrame.unmap();
    
    if (image.isNull()) {
        return;
    }
    
    QImage copy = image.copy();

    // 应用镜像效果
    if (current_mirror_) {
        copy = copy.mirrored(true, false);
    }

    // 显示到预览标签
    QSize preview_size = ui->label_preview->size();
    QImage scaled = copy.scaled(preview_size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    ui->label_preview->setPixmap(QPixmap::fromImage(scaled));
}

void CameraSettingsDialog::set_camera_name(const std::string& name)
{
    int index = ui->comboBox_camera->findText(QString::fromStdString(name));
    if (index >= 0) {
        ui->comboBox_camera->setCurrentIndex(index);
    }
}

void CameraSettingsDialog::set_resolution(const std::string& resolution)
{
    int index = ui->comboBox_resolution->findText(QString::fromStdString(resolution));
    if (index >= 0) {
        ui->comboBox_resolution->setCurrentIndex(index);
    }
}

void CameraSettingsDialog::set_fps(int fps)
{
    QString fps_str = QString::number(fps);
    int index = ui->comboBox_fps->findText(fps_str);
    if (index >= 0) {
        ui->comboBox_fps->setCurrentIndex(index);
    }
}

void CameraSettingsDialog::set_pixel_format(const std::string& format)
{
    int index = ui->comboBox_format->findText(QString::fromStdString(format));
    if (index >= 0) {
        ui->comboBox_format->setCurrentIndex(index);
    }
}

void CameraSettingsDialog::set_mirror(bool mirror)
{
    ui->checkBox_mirror->setChecked(mirror);
}

void CameraSettingsDialog::update_preview_mirror(bool mirrored)
{
    current_mirror_ = mirrored;
    emit preview_mirror_changed(mirrored);
    // 镜像效果会在 onVideoFrameChanged 中应用
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

void CameraSettingsDialog::start_preview()
{
    // 启动摄像头预览
    restart_camera_preview();
}

void CameraSettingsDialog::on_pushButton_ok_clicked()
{
    // 停止预览
    stop_camera_preview();

    emit settings_changed();
    accept();
}

void CameraSettingsDialog::on_pushButton_cancel_clicked()
{
    // 停止预览
    stop_camera_preview();

    reject();
}

bool CameraSettingsDialog::eventFilter(QObject* watched, QEvent* event)
{
    QComboBox* comboBox = qobject_cast<QComboBox*>(watched);
    if (comboBox) {
        if (event->type() == QEvent::MouseButtonPress) {
            QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
            comboBoxClickCount_++;
            LOG_INFO("[CameraSettingsDialog] ComboBox mouse press event: " + comboBox->objectName().toStdString() + 
                     " count=" + std::to_string(comboBoxClickCount_) + 
                     " button=" + std::to_string(mouseEvent->button()) +
                     " pos=" + std::to_string(mouseEvent->pos().x()) + "," + std::to_string(mouseEvent->pos().y()));
        }
        else if (event->type() == QEvent::MouseButtonRelease) {
            QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
            LOG_INFO("[CameraSettingsDialog] ComboBox mouse release event: " + comboBox->objectName().toStdString() + 
                     " button=" + std::to_string(mouseEvent->button()) +
                     " pos=" + std::to_string(mouseEvent->pos().x()) + 
                     "," + std::to_string(mouseEvent->pos().y()));
        }
        else if (event->type() == QEvent::Show) {
            // 下拉框弹出窗口显示
            isComboBoxOpen_ = true;
            LOG_INFO("[CameraSettingsDialog] ComboBox popup shown: " + comboBox->objectName().toStdString());
        }
        else if (event->type() == QEvent::Hide) {
            // 下拉框弹出窗口隐藏
            isComboBoxOpen_ = false;
            LOG_INFO("[CameraSettingsDialog] ComboBox popup hidden: " + comboBox->objectName().toStdString());
        }
    }
    return QDialog::eventFilter(watched, event);
}

} // namespace live_assistant
