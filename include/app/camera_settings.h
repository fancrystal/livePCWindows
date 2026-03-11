#pragma once

#include <QDialog>
#include <memory>
#include <vector>
#include <string>
#include <utility>

QT_BEGIN_NAMESPACE
class QCamera;
class QMediaCaptureSession;
class QVideoSink;
class QVideoFrame;
QT_END_NAMESPACE

namespace Ui {
class CameraSettingsDialog;
}

namespace live_assistant {

class CameraSettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit CameraSettingsDialog(QWidget *parent = nullptr);
    ~CameraSettingsDialog();

    // 设置可用摄像头列表 (display_name, device_id)
    void set_available_cameras(const std::vector<std::pair<std::string, std::string>>& cameras);

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

    // 启动预览
    void start_preview();

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

    // 视频帧变化处理（用于预览镜像）
    void onVideoFrameChanged(const QVideoFrame& frame);

protected:
    // 事件过滤，用于调试下拉框点击
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    Ui::CameraSettingsDialog *ui;

    // Qt Multimedia 摄像头相关对象
    QMediaCaptureSession* media_capture_session_;
    QCamera* camera_;
    QVideoSink* video_sink_;

    // 当前镜像状态
    bool current_mirror_ = false;

    // 调试计数器
    int comboBoxClickCount_ = 0;

    // 下拉框是否打开（用于暂停预览更新）
    bool isComboBoxOpen_ = false;

    // 停止摄像头预览
    void stop_camera_preview();

    // 重启摄像头预览（摄像头切换时调用）
    void restart_camera_preview();

    // 更新预览镜像效果
    void update_preview_mirror(bool mirrored);
};

} // namespace live_assistant
