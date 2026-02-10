#include "audio_engine/audio_capturer.h"
#include "common/log.h"

#include <QMediaDevices>
#include <QDateTime>
#include <QIODevice>
#include <QFile>

namespace live_assistant {

AudioCapturer::AudioCapturer(QObject* parent)
    : QObject(parent)
    , current_device_index_(-1)
    , is_capturing_(false)
    , sample_rate_(0)
    , channels_(0)
    , sample_size_(0) {
    LOG_INFO("[AudioCapturer] Created");
}

AudioCapturer::~AudioCapturer() {
    stop_capture();
    LOG_INFO("[AudioCapturer] Destroyed");
}

bool AudioCapturer::start_capture(int sample_rate, int channels, int sample_size) {
    // 停止当前采集
    stop_capture();

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
        LOG_INFO("[AudioCapturer] No audio device set, using default");
        audio_device_ = QMediaDevices::defaultAudioInput();
    }

    // 打印设备支持的格式
    QAudioFormat preferred_format = audio_device_.preferredFormat();
    LOG_INFO("[AudioCapturer] Device preferred format: " +
             std::to_string(preferred_format.sampleRate()) + "Hz, " +
             std::to_string(preferred_format.channelCount()) + " ch, " +
             std::to_string(preferred_format.bytesPerSample() * 8) + " bit, " +
             (preferred_format.sampleFormat() == QAudioFormat::Int16 ? "Int16" :
              preferred_format.sampleFormat() == QAudioFormat::Float ? "Float" : "UInt8"));

    // 检查格式支持
    if (!audio_device_.isFormatSupported(format)) {
        LOG_WARNING("[AudioCapturer] Desired format not supported, using nearest match");
        format = audio_device_.preferredFormat();
        LOG_INFO("[AudioCapturer] Using nearest match: " +
                 std::to_string(format.sampleRate()) + "Hz, " +
                 std::to_string(format.channelCount()) + " ch, " +
                 std::to_string(format.bytesPerSample() * 8) + " bit");
    }

    // 创建音频输入对象
    audio_input_ = std::make_unique<QAudioSource>(audio_device_, format, this);
    if (!audio_input_) {
        LOG_ERROR("[AudioCapturer] Failed to create QAudioSource");
        return false;
    }

    // 设置音频缓冲区大小（100ms）
    int buffer_size = format.sampleRate() * format.channelCount() *
                      format.bytesPerSample() / 10;
    audio_input_->setBufferSize(buffer_size);
    LOG_INFO("[AudioCapturer] Buffer size: " + std::to_string(buffer_size) + " bytes");

    // 启动采集
    audio_io_device_ = audio_input_->start();
    if (!audio_io_device_) {
        LOG_ERROR("[AudioCapturer] Failed to start audio capture");
        audio_input_.reset();
        return false;
    }

    // 连接数据就绪信号
    connect(audio_io_device_, &QIODevice::readyRead,
            this, &AudioCapturer::on_audio_ready_read);

    sample_rate_ = format.sampleRate();
    channels_ = format.channelCount();
    sample_size_ = format.bytesPerSample() * 8;
    is_capturing_ = true;

    LOG_INFO("[AudioCapturer] Started: " + audio_device_.description().toStdString() +
             ", " + std::to_string(sample_rate_) + "Hz, " +
             std::to_string(channels_) + " ch, " +
             std::to_string(sample_size_) + " bit");

    return true;
}

void AudioCapturer::stop_capture() {
    if (is_capturing_ && audio_input_) {
        audio_input_->stop();

        if (audio_io_device_) {
            disconnect(audio_io_device_, &QIODevice::readyRead,
                       this, &AudioCapturer::on_audio_ready_read);
        }

        audio_io_device_ = nullptr;
        audio_input_.reset();
        is_capturing_ = false;

        LOG_INFO("[AudioCapturer] Stopped");
    }
}

QStringList AudioCapturer::get_available_devices() const {
    QStringList device_names;
    const auto devices = QMediaDevices::audioInputs();

    for (const QAudioDevice& device : devices) {
        device_names << device.description();
        LOG_INFO("[AudioCapturer] Available device: " + device.description().toStdString());
    }

    return device_names;
}

bool AudioCapturer::switch_device(int device_index) {
    const QList<QAudioDevice> devices = QMediaDevices::audioInputs();

    if (device_index < 0 || device_index >= devices.size()) {
        LOG_ERROR("[AudioCapturer] Invalid device index: " + std::to_string(device_index));
        return false;
    }

    current_device_index_ = device_index;

    // 如果正在采集，则重启采集
    if (is_capturing_) {
        return start_capture(sample_rate_, channels_, sample_size_);
    }

    return true;
}

void AudioCapturer::on_audio_ready_read() {
    if (!audio_io_device_ || !is_capturing_) {
        return;
    }

    // 读取音频数据
    QByteArray data = audio_io_device_->readAll();
    if (data.isEmpty()) {
        return;
    }

    // 🔇 调试PCM文件写入已禁用（避免影响性能和磁盘空间）
    // 如需调试，可取消以下代码的注释：
    // static QFile pcmFile("debug_audio.pcm");
    // static bool pcmFileOpened = false;
    // if (!pcmFileOpened) {
    //     QFile::remove("debug_audio.pcm");
    //     if (pcmFile.open(QIODevice::WriteOnly)) {
    //         pcmFileOpened = true;
    //         LOG_INFO("[AudioCapturer] PCM debug file opened");
    //     }
    // }
    // if (pcmFileOpened && !data.isEmpty()) {
    //     pcmFile.write(data);
    //     pcmFile.flush();
    // }

    // 获取时间戳
    int64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // 发送信号，将音频数据传递出去
    emit data_captured(std::move(data), timestamp);
}

void AudioCapturer::set_audio_device(const QAudioDevice& audio_device) {
    if (is_capturing_) {
        stop_capture();
    }

    audio_device_ = audio_device;

    if (audio_device_.isNull()) {
        audio_input_.reset();
        audio_io_device_ = nullptr;
        is_capturing_ = false;
    }
}

} // namespace live_assistant
