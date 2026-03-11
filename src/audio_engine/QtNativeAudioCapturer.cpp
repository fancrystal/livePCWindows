#include "audio_engine/QtNativeAudioCapturer.h"
#include "common/log.h"

#include <QMediaDevices>
#include <QDateTime>
#include <QIODevice>

namespace live_assistant {

QtNativeAudioCapturer::QtNativeAudioCapturer(QObject* parent)
    : QObject(parent)
    , audio_io_device_(nullptr)
    , fixed_frame_timer_(nullptr)
    , is_capturing_(false) {
    LOG_INFO("[QtNativeAudioCapturer] Created");
}

QtNativeAudioCapturer::~QtNativeAudioCapturer() {
    Stop();
    LOG_INFO("[QtNativeAudioCapturer] Destroyed");
}

bool QtNativeAudioCapturer::Start(int sample_rate, int channels, int sample_size) {
    // 停止当前采集
    Stop();

    sample_rate_ = sample_rate;
    channels_ = channels;
    sample_size_ = sample_size;

    return initQtCapture(sample_rate, channels, sample_size);
}

void QtNativeAudioCapturer::Stop() {
    if (!is_capturing_) {
        return;
    }

    // 停止定时器
    if (fixed_frame_timer_) {
        fixed_frame_timer_->stop();
        delete fixed_frame_timer_;
        fixed_frame_timer_ = nullptr;
    }

    // 停止 Qt 音频采集
    if (audio_input_) {
        audio_input_->stop();

        if (audio_io_device_) {
            disconnect(audio_io_device_, &QIODevice::readyRead,
                       this, &QtNativeAudioCapturer::onAudioReadyRead);
        }

        audio_io_device_ = nullptr;
        audio_input_.reset();
        audio_buffer_.clear();
    }

    is_capturing_ = false;
    LOG_INFO("[QtNativeAudioCapturer] Stopped");
}

QStringList QtNativeAudioCapturer::GetAvailableDevices() {
    QStringList device_names;
    const auto devices = QMediaDevices::audioInputs();

    for (const QAudioDevice& device : devices) {
        device_names << device.description();
        LOG_INFO("[QtNativeAudioCapturer] Available device: " + device.description().toStdString());
    }

    return device_names;
}

void QtNativeAudioCapturer::SetAudioDevice(const QAudioDevice& device) {
    if (is_capturing_) {
        Stop();
    }

    audio_device_ = device;

    if (audio_device_.isNull()) {
        audio_input_.reset();
        audio_io_device_ = nullptr;
        is_capturing_ = false;
    }
}

bool QtNativeAudioCapturer::initQtCapture(int sample_rate, int channels, int sample_size) {
    LOG_INFO("[QtNativeAudioCapturer] Initializing Qt audio capture...");

    // 设置音频格式
    QAudioFormat format;
    format.setSampleRate(sample_rate);
    format.setChannelCount(channels);

    if (sample_size == 16) {
        format.setSampleFormat(QAudioFormat::Int16);
    } else if (sample_size == 32) {
        format.setSampleFormat(QAudioFormat::Float);
    } else {
        format.setSampleFormat(QAudioFormat::UInt8);
    }

    // 如果没有设置设备，使用默认设备
    if (audio_device_.isNull()) {
        LOG_INFO("[QtNativeAudioCapturer] No audio device set, using default");
        audio_device_ = QMediaDevices::defaultAudioInput();
    }

    // 打印设备支持的格式
    QAudioFormat preferred_format = audio_device_.preferredFormat();
    LOG_INFO("[QtNativeAudioCapturer] Device preferred format: " +
             std::to_string(preferred_format.sampleRate()) + "Hz, " +
             std::to_string(preferred_format.channelCount()) + " ch, " +
             std::to_string(preferred_format.bytesPerSample() * 8) + " bit, " +
             (preferred_format.sampleFormat() == QAudioFormat::Int16 ? "Int16" :
              preferred_format.sampleFormat() == QAudioFormat::Float ? "Float" : "UInt8"));

    // 检查格式支持
    if (!audio_device_.isFormatSupported(format)) {
        LOG_WARNING("[QtNativeAudioCapturer] Desired format not supported, using nearest match");
        format = audio_device_.preferredFormat();
        LOG_INFO("[QtNativeAudioCapturer] Using nearest match: " +
                 std::to_string(format.sampleRate()) + "Hz, " +
                 std::to_string(format.channelCount()) + " ch, " +
                 std::to_string(format.bytesPerSample() * 8) + " bit");
    }

    // 创建音频输入对象
    audio_input_ = std::make_unique<QAudioSource>(audio_device_, format, this);
    if (!audio_input_) {
        LOG_ERROR("[QtNativeAudioCapturer] Failed to create QAudioSource");
        return false;
    }

    // 🔧 更新实际使用的参数（格式可能被 Qt 调整了）
    sample_rate_ = format.sampleRate();
    channels_ = format.channelCount();
    sample_size_ = format.bytesPerSample() * 8;  // 更新为实际使用的位数

    // 打印实际使用的格式
    LOG_INFO("[QtNativeAudioCapturer] Using actual format: " +
             std::to_string(sample_rate_) + "Hz, " +
             std::to_string(channels_) + " ch, " +
             std::to_string(sample_size_) + " bit");

    // 设置音频缓冲区大小（100ms）
    int buffer_size = format.sampleRate() * format.channelCount() *
                      format.bytesPerSample() / 10;
    audio_input_->setBufferSize(buffer_size);
    LOG_INFO("[QtNativeAudioCapturer] Buffer size: " + std::to_string(buffer_size) + " bytes");

    // 启动采集
    audio_io_device_ = audio_input_->start();
    if (!audio_io_device_) {
        LOG_ERROR("[QtNativeAudioCapturer] Failed to start audio capture");
        audio_input_.reset();
        return false;
    }

    is_capturing_ = true;

    // Qt 模式：使用定时器固定帧输出
    fixed_frame_timer_ = new QTimer(this);
    connect(fixed_frame_timer_, &QTimer::timeout,
            this, &QtNativeAudioCapturer::onFixedFrameTimer);
    // 每 21ms 输出一次（约等于 1024 samples @ 48kHz）
    fixed_frame_timer_->start(21);

    // 连接信号
    connect(audio_io_device_, &QIODevice::readyRead,
            this, &QtNativeAudioCapturer::onAudioReadyRead);

    LOG_INFO("[QtNativeAudioCapturer] Qt audio capture started: " + audio_device_.description().toStdString() +
             ", " + std::to_string(sample_rate_) + "Hz, " +
             std::to_string(channels_) + " ch, " +
             std::to_string(sample_size_) + " bit");

    return true;
}

void QtNativeAudioCapturer::onAudioReadyRead() {
    if (!audio_io_device_ || !is_capturing_) {
        return;
    }

    // 读取音频数据
    QByteArray data = audio_io_device_->readAll();
    if (data.isEmpty()) {
        return;
    }

    // 获取时间戳
    int64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // 发送信号，将音频数据传递出去
    emit dataReady(std::move(data), timestamp);
}

void QtNativeAudioCapturer::onFixedFrameTimer() {
    if (!audio_io_device_ || !is_capturing_) {
        return;
    }

    // 1. 从 Qt 音频设备读取所有可用数据，累积到缓冲区
    QByteArray data = audio_io_device_->readAll();
    if (!data.isEmpty()) {
        // 将数据追加到缓冲区
        const uint8_t* src = reinterpret_cast<const uint8_t*>(data.constData());
        audio_buffer_.insert(audio_buffer_.end(), src, src + data.size());
    }

    // 2. 计算固定帧大小（字节）
    // 1024 samples * channels * bytes_per_sample
    size_t frame_size_bytes = AUDIO_FRAMES_PER_CALLBACK * channels_ * (sample_size_ / 8);

    // 3. 每次取出固定大小的帧发送
    while (audio_buffer_.size() >= frame_size_bytes) {
        // 创建固定大小的帧
        QByteArray frame_data(static_cast<int>(frame_size_bytes), Qt::Uninitialized);
        for (size_t i = 0; i < frame_size_bytes; i++) {
            frame_data[static_cast<int>(i)] = static_cast<char>(audio_buffer_[i]);
        }

        // 获取时间戳
        int64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        // 如果设置了回调，调用回调
        if (callback_) {
            // 根据实际 sample_size_ 处理数据
            if (sample_size_ == 32) {
                // Float32 格式：直接传递
                const float* float_data = reinterpret_cast<const float*>(frame_data.constData());
                callback_(float_data, AUDIO_FRAMES_PER_CALLBACK,
                         sample_rate_, channels_, timestamp * 1000000);
            } else if (sample_size_ == 16) {
                // Int16 格式：转换为 float
                static std::vector<float> float_buffer(AUDIO_FRAMES_PER_CALLBACK * 2); // 最大立体声
                const int16_t* int16_data = reinterpret_cast<const int16_t*>(frame_data.constData());
                int num_samples = AUDIO_FRAMES_PER_CALLBACK * channels_;
                if (float_buffer.size() < (size_t)num_samples) {
                    float_buffer.resize(num_samples);
                }
                for (int i = 0; i < num_samples; i++) {
                    float_buffer[i] = int16_data[i] / 32768.0f;  // 转换为 -1.0 到 1.0
                }
                callback_(float_buffer.data(), AUDIO_FRAMES_PER_CALLBACK,
                         sample_rate_, channels_, timestamp * 1000000);
            } else {
                LOG_WARNING("[QtNativeAudioCapturer] Unsupported sample size: " + std::to_string(sample_size_));
            }
        }

        // 从缓冲区移除已发送的数据
        audio_buffer_.erase(audio_buffer_.begin(), 
                            audio_buffer_.begin() + frame_size_bytes);
    }
}

} // namespace live_assistant
