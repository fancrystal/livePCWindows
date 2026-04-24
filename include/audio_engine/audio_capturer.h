#pragma once

#include <QObject>
#include <QAudioDevice>
#include <memory>
#include <QStringList>
#include "wasapi_capturer.h"

namespace live_assistant {

// 前向声明
class QtNativeAudioCapturer;

/**
 * @brief 音频采集模式
 */
enum class AudioCaptureMode {
    QT_CALLBACK,    // Qt 默认方式：数据驱动，帧大小不固定
    FN_STYLE        // FN 方式：WASAPI 采集，固定帧大小
};

/**
 * @brief 纯音频捕获类
 *
 * 负责：
 * - 使用 WASAPI 或 QtNativeAudioCapturer 进行音频捕获
 * - 发射 dataCaptured 信号传递 QByteArray
 *
 * 不负责：
 * - 音量控制
 * - 静音控制
 * - 音频帧队列管理
 */
class AudioCapturer : public QObject {
    Q_OBJECT

public:
    explicit AudioCapturer(QObject* parent = nullptr);
    ~AudioCapturer() override;

    // 开始音频采集
    bool start_capture(int sample_rate = 48000, int channels = 2, int sample_size = 16);

    // 停止音频采集
    void stop_capture();

    // 获取可用的音频输入设备
    QStringList get_available_devices() const;

    // 切换音频输入设备
    bool switch_device(int device_index);

    // 是否正在采集
    bool is_capturing() const { return is_capturing_; }

    // 获取采集参数
    int get_sample_rate() const { return sample_rate_; }
    int get_channels() const { return channels_; }
    int get_sample_size() const { return sample_size_; }

    // 设置音频设备（暂未实现）
    void set_audio_device(const QAudioDevice& audio_device);

    // 获取当前设备（暂未实现）
    QAudioDevice get_audio_device() const { return QAudioDevice(); }
    
    // 设置采集模式
    void set_capture_mode(AudioCaptureMode mode) { capture_mode_ = mode; }
    AudioCaptureMode get_capture_mode() const { return capture_mode_; }
    
    // 设置 WASAPI 设备 ID（FN 模式使用）
    void set_wasapi_device_id(const std::string& device_id) { wasapi_device_id_ = device_id; }
    void set_speaker_wasapi_device_id(const std::string& device_id) { speaker_wasapi_device_id_ = device_id; }
    
    // 获取 WASAPI 设备列表
    static std::vector<AudioDeviceInfo> get_wasapi_devices();

signals:
    // 音频数据捕获信号（麦克风）
    // 参数：音频数据(QByteArray移动语义)、时间戳
    void data_captured(QByteArray data, int64_t timestamp);
    
    // 音频数据捕获信号（扬声器/桌面音频）
    // 参数：音频数据(QByteArray移动语义)、时间戳
    void speaker_data_captured(QByteArray data, int64_t timestamp);

private:
    // 初始化 WASAPI 采集
    bool init_wasapi_capture();
    
    // 初始化 Qt Native 音频采集
    bool init_qt_native_capture();
    
    // 音频数据回调（麦克风）
    void on_wasapi_data(const float* data, uint32_t frames, 
                        uint32_t sample_rate, uint32_t channels,
                        int64_t timestamp);
    
    // 音频数据回调（扬声器/桌面音频）
    void on_speaker_data(const float* data, uint32_t frames,
                         uint32_t sample_rate, uint32_t channels,
                         int64_t timestamp);

public:
    // 设置扬声器采集开关
    void set_speaker_capture_enabled(bool enabled);
    bool is_speaker_capture_enabled() const { return speaker_capture_enabled_; }

private:
    // WASAPI 音频采集（默认）- 麦克风
    std::unique_ptr<WASAPICapturer> wasapi_capturer_;
    std::string wasapi_device_id_;

    // WASAPI 音频采集 - 扬声器（桌面音频）
    std::unique_ptr<WASAPICapturer> speaker_capturer_;
    bool speaker_capture_enabled_ = false;
    
    // Qt Native 音频采集（可选）
    std::string speaker_wasapi_device_id_;

    bool start_speaker_capture();
    void stop_speaker_capture();

    std::unique_ptr<QtNativeAudioCapturer> qt_native_capturer_;

    int current_device_index_ = -1;
    bool is_capturing_ = false;

    int sample_rate_ = 0;
    int channels_ = 0;
    int sample_size_ = 0;
    
    // 采集模式（默认使用 FN_STYLE）
    AudioCaptureMode capture_mode_ = AudioCaptureMode::FN_STYLE;
    
    // 固定帧大小
    static constexpr int AUDIO_FRAMES_PER_CALLBACK = 1024;
};

} // namespace live_assistant
