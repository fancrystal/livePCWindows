#pragma once

#include <QDialog>
#include <QImage>
#include <memory>
#include <vector>
#include <string>
#include <utility>
#include "scene_manager/icapture_source.h"

QT_BEGIN_NAMESPACE
class QTimer;
class QProgressBar;
QT_END_NAMESPACE

namespace Ui {
class CameraSettingsDialog;
}

namespace live_assistant {

class ICaptureSource;
struct CaptureFrame;
class CameraCapabilityScanner;

class CameraSettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit CameraSettingsDialog(QWidget *parent = nullptr);
    ~CameraSettingsDialog();

    // 设置可用摄像头列表 (display_name, device_id)
    void set_available_cameras(const std::vector<std::pair<std::string, std::string>>& cameras);

    // 设置可用摄像头列表（包含 OpenCV 索引）
    void set_available_cameras_with_opencv(
        const std::vector<std::string>& display_names,
        const std::vector<std::string>& dshow_names,
        const std::vector<int>& opencv_indices);

    // 设置摄像头名称
    void set_camera_name(const std::string& name);

    // 设置分辨率
    void set_resolution(const std::string& resolution);

    // 设置帧率
    void set_fps(int fps);

    // 设置像素格式
    void set_pixel_format(const std::string& format);

    // 设置画面镜像
    void set_mirror(bool mirror);

    // 设置采集模式
    void set_capture_mode(CaptureMode mode);

    // 获取摄像头名称（用于显示）
    std::string get_camera_name() const;

    // 获取摄像头设备标识
    std::string get_camera_device_id() const;

    // 获取分辨率
    std::string get_resolution() const;

    // 获取帧率
    int get_fps() const;

    // 获取像素格式
    std::string get_pixel_format() const;

    // 获取画面镜像状态
    bool is_mirror() const;

    // 获取采集模式
    CaptureMode get_capture_mode() const;

    // 获取完整的采集配置
    CaptureConfig get_capture_config() const;

    // 获取预览用的采集源（确定后可以复用，避免重复打开摄像头）
    std::shared_ptr<ICaptureSource> take_preview_source();

    // 设置已有的采集源用于预览（画布上已有摄像头时使用）
    void set_existing_source(std::shared_ptr<ICaptureSource> source);

    // 启动预览（创建临时采集源或使用已有源）
    bool start_preview(const std::string& device_id = "");

    // 停止并销毁预览源
    void stop_preview();

    // 设置是否禁用预览（当摄像头已被占用时使用）
    void set_preview_disabled(bool disabled);

signals:
    // 设置改变信号
    void settings_changed();

    // 镜像状态改变信号
    void mirror_changed(bool mirrored);

    // 预览镜像改变信号
    void preview_mirror_changed(bool mirrored);

private slots:
    // 确定按钮点击
    void on_pushButton_ok_clicked();

    // 取消按钮点击
    void on_pushButton_cancel_clicked();

    // 预览帧更新
    void on_preview_frame(const CaptureFrame& frame);

    // 摄像头选择变化
    void on_camera_changed(int index);

    // 采集模式变化
    void on_capture_mode_changed(int index);

    // 分辨率选择变化
    void on_resolution_changed(int index);

    // 能力查询完成
    void on_capabilities_ready(const QString& device_id, const CameraCapabilities& caps);

private:
    Ui::CameraSettingsDialog *ui;

    // 能力扫描器
    CameraCapabilityScanner* capability_scanner_ = nullptr;

    // 当前能力集
    CameraCapabilities current_capabilities_;

    // 预览用的采集源
    std::shared_ptr<ICaptureSource> preview_source_;

    // 是否使用已有源（不是临时创建的）
    bool using_existing_source_ = false;

    // 当前镜像状态
    bool current_mirror_ = false;

    // 调试计数器
    int comboBoxClickCount_ = 0;

    // 正在查询能力
    bool is_querying_capabilities_ = false;

    // 是否禁用预览（摄像头已被占用时）
    bool preview_disabled_ = false;

    // OpenCV 索引列表（与摄像头列表对应）
    std::vector<int> opencv_indices_;

    // 更新预览镜像效果
    void update_preview_mirror(bool mirrored);

    // 查询摄像头能力
    void query_camera_capabilities(const std::string& device_id);

    // 更新分辨率下拉框
    void update_resolution_combo();

    // 更新帧率下拉框
    void update_fps_combo();

    // 更新像素格式下拉框
    void update_format_combo();

    // 根据选中的分辨率更新可用的帧率
    void update_available_fps();

    // 根据选中的分辨率和帧率更新可用的像素格式
    void update_available_formats();

    // 查找匹配的模式
    CameraMode find_matching_mode(int width, int height, int fps) const;

protected:
    // 事件过滤，用于调试下拉框点击
    bool eventFilter(QObject* watched, QEvent* event) override;
};

} // namespace live_assistant