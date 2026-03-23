#include "app/camera_settings.h"
#include "ui_camera_settings.h"
#include "scene_manager/icapture_source.h"
#include "scene_manager/capture_factory.h"
#include "app/camera_capability_scanner.h"
#include <QImage>
#include <QEvent>
#include <QMouseEvent>
#include <QComboBox>
#include <QTimer>
#include <QMessageBox>
#include <set>
#include "common/log.h"

namespace live_assistant {

CameraSettingsDialog::CameraSettingsDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::CameraSettingsDialog),
    capability_scanner_(new CameraCapabilityScanner(this)),
    using_existing_source_(false),
    current_mirror_(false),
    comboBoxClickCount_(0),
    is_querying_capabilities_(false)
{
    ui->setupUi(this);

    // 设置默认值
    set_resolution("640x360");
    set_fps(30);
    set_pixel_format("YUY2");
    set_mirror(false);
    set_capture_mode(CaptureMode::FFMPEG);

    // 安装事件过滤器到所有下拉框
    ui->comboBox_camera->installEventFilter(this);
    ui->comboBox_captureMode->installEventFilter(this);
    ui->comboBox_resolution->installEventFilter(this);
    ui->comboBox_fps->installEventFilter(this);
    ui->comboBox_format->installEventFilter(this);

    // 连接能力扫描器信号
    connect(capability_scanner_, &CameraCapabilityScanner::capabilities_ready,
            this, &CameraSettingsDialog::on_capabilities_ready);

    // 连接摄像头选择变化
    connect(ui->comboBox_camera, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CameraSettingsDialog::on_camera_changed);

    // 连接采集模式选择变化
    connect(ui->comboBox_captureMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CameraSettingsDialog::on_capture_mode_changed);

    // 连接分辨率选择变化
    connect(ui->comboBox_resolution, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CameraSettingsDialog::on_resolution_changed);

    // 连接镜像复选框状态变化
    connect(ui->checkBox_mirror, &QCheckBox::stateChanged, this, [this](int state) {
        bool mirrored = (state == Qt::Checked);
        emit mirror_changed(mirrored);
        update_preview_mirror(mirrored);
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

    // 设置默认选中基础设置页面
    ui->pushButton_basic->setChecked(true);
    ui->stackedWidget->setCurrentWidget(ui->page_basic);
}

CameraSettingsDialog::~CameraSettingsDialog()
{
    stop_preview();
    delete ui;
}

void CameraSettingsDialog::set_available_cameras(const std::vector<std::pair<std::string, std::string>>& cameras) {
    ui->comboBox_camera->clear();
    opencv_indices_.clear();

    for (const auto& cam : cameras) {
        ui->comboBox_camera->addItem(QString::fromStdString(cam.first), QString::fromStdString(cam.second));
        opencv_indices_.push_back(0);  // 默认 OpenCV 索引
    }

    if (!cameras.empty()) {
        ui->comboBox_camera->setCurrentIndex(0);

        // 自动查询第一个摄像头的能力
        std::string device_id = cameras[0].second;
        if (!device_id.empty()) {
            query_camera_capabilities(device_id);
        }
    }
}

void CameraSettingsDialog::set_available_cameras_with_opencv(
    const std::vector<std::string>& display_names,
    const std::vector<std::string>& dshow_names,
    const std::vector<int>& opencv_indices) {
    ui->comboBox_camera->clear();
    opencv_indices_.clear();

    for (size_t i = 0; i < display_names.size() && i < dshow_names.size(); ++i) {
        ui->comboBox_camera->addItem(QString::fromStdString(display_names[i]),
                                      QString::fromStdString(dshow_names[i]));
        opencv_indices_.push_back(i < opencv_indices.size() ? opencv_indices[i] : 0);
    }

    if (!display_names.empty()) {
        ui->comboBox_camera->setCurrentIndex(0);

        // 自动查询第一个摄像头的能力
        std::string device_id = dshow_names[0];
        if (!device_id.empty()) {
            query_camera_capabilities(device_id);
        }
    }
}

void CameraSettingsDialog::set_camera_name(const std::string& name) {
    int index = ui->comboBox_camera->findText(QString::fromStdString(name));
    if (index >= 0) {
        ui->comboBox_camera->setCurrentIndex(index);
    }
}

void CameraSettingsDialog::set_resolution(const std::string& resolution) {
    int index = ui->comboBox_resolution->findText(QString::fromStdString(resolution));
    if (index >= 0) {
        ui->comboBox_resolution->setCurrentIndex(index);
    }
}

void CameraSettingsDialog::set_fps(int fps) {
    QString fps_str = QString::number(fps);
    int index = ui->comboBox_fps->findText(fps_str);
    if (index >= 0) {
        ui->comboBox_fps->setCurrentIndex(index);
    }
}

void CameraSettingsDialog::set_pixel_format(const std::string& format) {
    int index = ui->comboBox_format->findText(QString::fromStdString(format));
    if (index >= 0) {
        ui->comboBox_format->setCurrentIndex(index);
    }
}

void CameraSettingsDialog::set_mirror(bool mirror) {
    ui->checkBox_mirror->setChecked(mirror);
}

void CameraSettingsDialog::set_capture_mode(CaptureMode mode) {
    int index = (mode == CaptureMode::OPENCV) ? 1 : 0;
    ui->comboBox_captureMode->setCurrentIndex(index);
    LOG_INFO("[CameraSettingsDialog] set_capture_mode: " + capture_mode_to_string(mode));
}

std::string CameraSettingsDialog::get_camera_name() const {
    return ui->comboBox_camera->currentText().toStdString();
}

std::string CameraSettingsDialog::get_camera_device_id() const {
    return ui->comboBox_camera->currentData().toString().toStdString();
}

std::string CameraSettingsDialog::get_resolution() const {
    return ui->comboBox_resolution->currentText().toStdString();
}

int CameraSettingsDialog::get_fps() const {
    return ui->comboBox_fps->currentText().toInt();
}

std::string CameraSettingsDialog::get_pixel_format() const {
    return ui->comboBox_format->currentText().toStdString();
}

bool CameraSettingsDialog::is_mirror() const {
    return ui->checkBox_mirror->isChecked();
}

CaptureMode CameraSettingsDialog::get_capture_mode() const {
    int index = ui->comboBox_captureMode->currentIndex();
    return (index == 1) ? CaptureMode::OPENCV : CaptureMode::FFMPEG;
}

CaptureConfig CameraSettingsDialog::get_capture_config() const {
    CaptureConfig config;
    config.type = CaptureConfig::TargetType::CAMERA;
    config.target_id = get_camera_device_id();
    config.display_name = get_camera_name();
    config.fps = get_fps();
    config.mirror = is_mirror();
    config.capture_mode = get_capture_mode();
    config.pixel_format = string_to_pixel_format(get_pixel_format());

    // 获取当前选中摄像头的 OpenCV 索引
    int camera_index = ui->comboBox_camera->currentIndex();
    if (camera_index >= 0 && camera_index < static_cast<int>(opencv_indices_.size())) {
        config.opencv_index = opencv_indices_[camera_index];
    }

    // 解析分辨率
    std::string resolution = get_resolution();
    size_t pos = resolution.find('x');
    if (pos != std::string::npos && pos > 0 && pos < resolution.length() - 1) {
        try {
            config.width = std::stoi(resolution.substr(0, pos));
            config.height = std::stoi(resolution.substr(pos + 1));
        } catch (...) {
            // 解析失败，使用默认值
            config.width = 640;
            config.height = 360;
        }
    } else {
        // 默认分辨率
        config.width = 640;
        config.height = 360;
    }

    return config;
}

bool CameraSettingsDialog::start_preview(const std::string& device_id) {
    LOG_INFO("[CameraSettingsDialog] start_preview() called, device_id=" + device_id);

    if (preview_source_ && preview_source_->is_running()) {
        LOG_INFO("[CameraSettingsDialog] preview source already running");
        return true;
    }

    if (!preview_source_) {
        CaptureConfig cfg = get_capture_config();
        std::string target_device = device_id.empty() ? cfg.target_id : device_id;

        if (target_device.empty()) {
            LOG_WARNING("[CameraSettingsDialog] no device selected for preview");
            return false;
        }

        cfg.target_id = target_device;
        LOG_INFO("[CameraSettingsDialog] creating preview source for device: " + target_device +
                 " mode=" + capture_mode_to_string(cfg.capture_mode));

        preview_source_ = CaptureFactory::create_capture_source(cfg);
        if (!preview_source_) {
            LOG_ERROR("[CameraSettingsDialog] failed to create preview source");
            return false;
        }

        using_existing_source_ = false;
        LOG_INFO("[CameraSettingsDialog] preview source created successfully");
    }

    connect(preview_source_.get(), &ICaptureSource::frameReady, this, &CameraSettingsDialog::on_preview_frame);

    if (!preview_source_->start()) {
        LOG_ERROR("[CameraSettingsDialog] failed to start preview source");
        return false;
    }

    LOG_INFO("[CameraSettingsDialog] preview started successfully");
    return true;
}

void CameraSettingsDialog::stop_preview() {
    LOG_INFO("[CameraSettingsDialog] stop_preview() called, preview_source_=" +
             std::string(preview_source_ ? "not null" : "null") +
             ", using_existing_source_=" + std::string(using_existing_source_ ? "true" : "false"));

    if (preview_source_) {
        disconnect(preview_source_.get(), &ICaptureSource::frameReady, this, &CameraSettingsDialog::on_preview_frame);

        if (!using_existing_source_) {
            LOG_INFO("[CameraSettingsDialog] stopping and destroying temporary preview source");
            preview_source_->stop();
            preview_source_->shutdown();
            preview_source_.reset();
        } else {
            LOG_INFO("[CameraSettingsDialog] keeping existing source, just stopping preview display");
        }
    }
}

void CameraSettingsDialog::set_existing_source(std::shared_ptr<ICaptureSource> source) {
    LOG_INFO("[CameraSettingsDialog] set_existing_source() called");
    preview_source_ = source;
    using_existing_source_ = true;
}

std::shared_ptr<ICaptureSource> CameraSettingsDialog::take_preview_source() {
    LOG_INFO("[CameraSettingsDialog] take_preview_source() called, using_existing_source_=" +
             std::string(using_existing_source_ ? "true" : "false"));

    if (using_existing_source_) {
        LOG_INFO("[CameraSettingsDialog] using existing source, returning nullptr");
        return nullptr;
    }

    auto source = preview_source_;
    preview_source_.reset();
    LOG_INFO("[CameraSettingsDialog] returning preview source for caller to take over");
    return source;
}

void CameraSettingsDialog::on_preview_frame(const CaptureFrame& frame) {
    if (frame.image.isNull()) {
        return;
    }

    QImage display_frame = frame.image;
    if (current_mirror_) {
        display_frame = frame.image.mirrored(true, false);
    }

    QSize preview_size = ui->label_preview->size();
    QImage scaled = display_frame.scaled(preview_size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    ui->label_preview->setPixmap(QPixmap::fromImage(scaled));
}

void CameraSettingsDialog::on_camera_changed(int index) {
    LOG_INFO("[CameraSettingsDialog] Camera changed to index " + std::to_string(index));

    if (index < 0) return;

    std::string device_id = get_camera_device_id();
    if (device_id.empty()) return;

    // 停止当前预览
    stop_preview();

    // 查询新摄像头的能力
    query_camera_capabilities(device_id);
}

void CameraSettingsDialog::on_capture_mode_changed(int index) {
    LOG_INFO("[CameraSettingsDialog] Capture mode changed to index " + std::to_string(index));

    // 重新查询能力
    std::string device_id = get_camera_device_id();
    if (!device_id.empty()) {
        stop_preview();
        query_camera_capabilities(device_id);
    }
}

void CameraSettingsDialog::on_resolution_changed(int index) {
    LOG_INFO("[CameraSettingsDialog] Resolution changed to index " + std::to_string(index));

    // 根据选中的分辨率更新可用的帧率
    update_available_fps();
}

void CameraSettingsDialog::on_capabilities_ready(const QString& device_id, const CameraCapabilities& caps) {
    LOG_INFO("[CameraSettingsDialog] Capabilities ready for " + device_id.toStdString() +
             ", modes count: " + std::to_string(caps.size()));

    is_querying_capabilities_ = false;
    current_capabilities_ = caps;

    // 更新下拉框
    update_resolution_combo();

    // 如果预览未被禁用，启动预览
    if (!preview_disabled_) {
        start_preview(device_id.toStdString());
    } else {
        LOG_INFO("[CameraSettingsDialog] Preview disabled, showing placeholder");
        ui->label_preview->setText("预览不可用\n（摄像头已被占用）");
    }
}

void CameraSettingsDialog::query_camera_capabilities(const std::string& device_id) {
    if (device_id.empty()) return;

    LOG_INFO("[CameraSettingsDialog] Querying capabilities for " + device_id);
    is_querying_capabilities_ = true;

    CaptureMode mode = get_capture_mode();
    capability_scanner_->query_capabilities_async(device_id, mode);
}

void CameraSettingsDialog::update_resolution_combo() {
    ui->comboBox_resolution->clear();

    if (current_capabilities_.empty()) {
        // 使用默认分辨率，第一项是默认选中项
        ui->comboBox_resolution->addItem("640x360");
        ui->comboBox_resolution->addItem("640x480");
        ui->comboBox_resolution->addItem("1280x720");
        ui->comboBox_resolution->addItem("1920x1080");

        // 更新帧率和格式
        update_available_fps();
        return;
    }

    // 收集所有唯一的分辨率
    std::vector<std::pair<int, int>> resolutions;
    for (const auto& mode : current_capabilities_.modes) {
        auto res = std::make_pair(mode.width, mode.height);
        if (std::find(resolutions.begin(), resolutions.end(), res) == resolutions.end()) {
            resolutions.push_back(res);
        }
    }

    // 按分辨率从小到大排序，这样常用分辨率在前面
    std::sort(resolutions.begin(), resolutions.end(), [](const auto& a, const auto& b) {
        return a.first * a.second < b.first * b.second;
    });

    for (const auto& res : resolutions) {
        QString res_str = QString("%1x%2").arg(res.first).arg(res.second);
        ui->comboBox_resolution->addItem(res_str);
    }

    // 默认选中 640x360（如果存在）
    int idx = ui->comboBox_resolution->findText("640x360");
    if (idx >= 0) {
        ui->comboBox_resolution->setCurrentIndex(idx);
    }

    // 更新帧率和格式
    update_available_fps();
}

void CameraSettingsDialog::update_fps_combo() {
    ui->comboBox_fps->clear();

    // 常见帧率
    ui->comboBox_fps->addItem("30");
    ui->comboBox_fps->addItem("25");
    ui->comboBox_fps->addItem("60");
    ui->comboBox_fps->addItem("15");
}

void CameraSettingsDialog::update_format_combo() {
    ui->comboBox_format->clear();

    std::string resolution = get_resolution();
    int fps = get_fps();

    // 解析分辨率
    int width = 0, height = 0;
    size_t pos = resolution.find('x');
    if (pos != std::string::npos && pos > 0 && pos < resolution.length() - 1) {
        try {
            width = std::stoi(resolution.substr(0, pos));
            height = std::stoi(resolution.substr(pos + 1));
        } catch (...) {
            // 解析失败，使用默认值
            width = 640;
            height = 360;
        }
    } else {
        // 分辨率格式无效，使用默认格式列表
        ui->comboBox_format->addItem("YUY2");
        ui->comboBox_format->addItem("MJPG");
        ui->comboBox_format->addItem("NV12");
        ui->comboBox_format->addItem("I420");
        return;
    }

    // 查找匹配的模式
    CameraMode mode = find_matching_mode(width, height, fps);

    if (mode.supported_formats.empty()) {
        // 使用默认格式，根据分辨率推荐
        if (width >= 1280) {
            // 高分辨率优先 MJPG
            ui->comboBox_format->addItem("MJPG");
            ui->comboBox_format->addItem("YUY2");
        } else {
            // 低分辨率优先 YUY2
            ui->comboBox_format->addItem("YUY2");
            ui->comboBox_format->addItem("MJPG");
        }
        ui->comboBox_format->addItem("NV12");
        ui->comboBox_format->addItem("I420");
    } else {
        for (PixelFormat fmt : mode.supported_formats) {
            ui->comboBox_format->addItem(QString::fromStdString(pixel_format_to_string(fmt)));
        }
    }
}

void CameraSettingsDialog::update_available_fps() {
    // 根据选中的分辨率更新可用的帧率
    std::string resolution = get_resolution();

    std::set<int> available_fps;
    if (!current_capabilities_.empty()) {
        size_t pos = resolution.find('x');
        if (pos != std::string::npos) {
            int width = std::stoi(resolution.substr(0, pos));
            int height = std::stoi(resolution.substr(pos + 1));

            for (const auto& mode : current_capabilities_.modes) {
                if (mode.width == width && mode.height == height) {
                    available_fps.insert(mode.fps);
                }
            }
        }
    }

    ui->comboBox_fps->clear();
    if (available_fps.empty()) {
        ui->comboBox_fps->addItem("30");
        ui->comboBox_fps->addItem("25");
        ui->comboBox_fps->addItem("60");
        ui->comboBox_fps->addItem("15");
    } else {
        for (int fps : available_fps) {
            ui->comboBox_fps->addItem(QString::number(fps));
        }
    }

    // 更新格式
    update_available_formats();
}

void CameraSettingsDialog::update_available_formats() {
    update_format_combo();
}

CameraMode CameraSettingsDialog::find_matching_mode(int width, int height, int fps) const {
    for (const auto& mode : current_capabilities_.modes) {
        if (mode.width == width && mode.height == height && mode.fps == fps) {
            return mode;
        }
    }
    return CameraMode();
}

void CameraSettingsDialog::update_preview_mirror(bool mirrored) {
    current_mirror_ = mirrored;
    emit preview_mirror_changed(mirrored);
}

void CameraSettingsDialog::set_preview_disabled(bool disabled) {
    preview_disabled_ = disabled;
    if (disabled) {
        ui->label_preview->setText("预览不可用\n（摄像头已被占用）");
    }
}

void CameraSettingsDialog::on_pushButton_ok_clicked() {
    emit settings_changed();
    accept();
}

void CameraSettingsDialog::on_pushButton_cancel_clicked() {
    stop_preview();
    reject();
}

bool CameraSettingsDialog::eventFilter(QObject* watched, QEvent* event) {
    QComboBox* comboBox = qobject_cast<QComboBox*>(watched);
    if (comboBox) {
        if (event->type() == QEvent::MouseButtonPress) {
            QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
            comboBoxClickCount_++;
            LOG_INFO("[CameraSettingsDialog] ComboBox mouse press event: " + comboBox->objectName().toStdString() +
                     " count=" + std::to_string(comboBoxClickCount_));
        }
        else if (event->type() == QEvent::Show) {
            LOG_INFO("[CameraSettingsDialog] ComboBox popup shown: " + comboBox->objectName().toStdString());
        }
        else if (event->type() == QEvent::Hide) {
            LOG_INFO("[CameraSettingsDialog] ComboBox popup hidden: " + comboBox->objectName().toStdString());
        }
    }
    return QDialog::eventFilter(watched, event);
}

} // namespace live_assistant