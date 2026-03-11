#pragma once

#include <QObject>
#include <QAudioSource>
#include <QAudioDevice>
#include <QTimer>
#include <vector>
#include <deque>
#include <functional>

#include "audio_engine/audio_engine.h"

namespace live_assistant {

constexpr int AUDIO_FRAMES_PER_CALLBACK = 1024;

// Qt 原生音频采集器
class QtNativeAudioCapturer : public QObject {
    Q_OBJECT

public:
    explicit QtNativeAudioCapturer(QObject* parent = nullptr);
    ~QtNativeAudioCapturer();

    // 启动采集
    bool Start(int sample_rate, int channels, int sample_size);

    // 停止采集
    void Stop();

    // 是否正在采集
    bool IsCapturing() const { return is_capturing_; }

    // 获取采集参数
    int GetSampleRate() const { return sample_rate_; }
    int GetChannels() const { return channels_; }
    int GetSampleSize() const { return sample_size_; }

    // 获取可用设备列表
    static QStringList GetAvailableDevices();

    // 设置音频设备
    void SetAudioDevice(const QAudioDevice& device);

    // 音频数据回调类型
    using AudioDataCallback = std::function<void(const float* data, uint32_t frames,
                                                  uint32_t sample_rate, uint32_t channels,
                                                  int64_t timestamp)>;
    void SetCallback(AudioDataCallback callback) { callback_ = callback; }

signals:
    // Qt 信号：采集到的音频数据
    void dataReady(const QByteArray& data, int64_t timestamp);

private slots:
    void onAudioReadyRead();

private:
    // 初始化 Qt 音频采集
    bool initQtCapture(int sample_rate, int channels, int sample_size);

    // 处理定时器事件（固定帧输出）
    void onFixedFrameTimer();

    bool is_capturing_ = false;
    int sample_rate_ = 0;
    int channels_ = 0;
    int sample_size_ = 0;

    QAudioDevice audio_device_;
    std::unique_ptr<QAudioSource> audio_input_;
    QIODevice* audio_io_device_ = nullptr;

    // 定时器（用于固定帧输出）
    QTimer* fixed_frame_timer_ = nullptr;

    // 音频缓冲（用于累积数据并输出固定帧）
    std::deque<uint8_t> audio_buffer_;

    // 回调函数
    AudioDataCallback callback_;
};

} // namespace live_assistant
