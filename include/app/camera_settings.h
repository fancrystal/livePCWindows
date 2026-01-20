#pragma once

#include <QDialog>
#include <memory>
#include <vector>
#include <string>

#include "video_engine/video_engine.h"

namespace Ui {
class CameraSettingsDialog;
}

namespace live_assistant {

class CameraSettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit CameraSettingsDialog(QWidget *parent = nullptr);
    ~CameraSettingsDialog();

    // 设置可用摄像头列表（仅显示名；兼容接口）
    void set_available_cameras(const std::vector<std::string>& cameras);

    // 设置可用摄像头列表（显示名 + dshow device_name）
    void set_available_camera_choices(const std::vector<VideoEngine::CameraChoice>& cameras);
    
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
    
    // 设置摄像头开关状态
    void set_camera_enabled(bool enabled);
    
    // 设置圆角
    void set_corner_rounding(bool enabled);
    
    // 获取摄像头名称（用于显示）
    std::string get_camera_name() const;

    // 获取摄像头设备标识（dshow device_name 或 OpenCV fallback index）
    std::string get_camera_device_id() const;
    
    // 获取分辨率
    std::string get_resolution() const;
    
    // 获取帧率
    int get_fps() const;
    
    // 获取像素格式
    std::string get_pixel_format() const;
    
    // 获取画面镜像状态
    bool is_mirror() const;
    
    // 获取摄像头开关状态
    bool is_camera_enabled() const;
    
    // 获取圆角状态
    bool is_corner_rounding() const;

signals:
    // 设置改变信号
    void settings_changed();

private slots:
    // 确定按钮点击
    void on_pushButton_ok_clicked();
    
    // 取消按钮点击
    void on_pushButton_cancel_clicked();

private:
    Ui::CameraSettingsDialog *ui;
};

} // namespace live_assistant