#pragma once

#include <QObject>
#include <QAudioFormat>
#include <QIODevice>
#include <QAudioDevice>
#include <QAudioSource>
#include <memory>
#include <QStringList>

namespace live_assistant {

/**
 * @brief 纯音频捕获类（从原项目移植）
 *
 * 负责：
 * - 使用 QAudioSource 进行音频捕获
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

    // 设置音频设备
    void set_audio_device(const QAudioDevice& audio_device);

    // 获取当前设备
    QAudioDevice get_audio_device() const { return audio_device_; }

signals:
    // 音频数据捕获信号
    // 参数：音频数据(QByteArray移动语义)、时间戳
    void data_captured(QByteArray data, int64_t timestamp);

private slots:
    // 处理音频数据就绪事件
    void on_audio_ready_read();

private:
    std::unique_ptr<QAudioSource> audio_input_;
    QAudioDevice audio_device_;
    QIODevice* audio_io_device_ = nullptr;
    QAudioFormat audio_format_;

    int current_device_index_ = -1;
    bool is_capturing_ = false;

    int sample_rate_ = 0;
    int channels_ = 0;
    int sample_size_ = 0;
};

} // namespace live_assistant
