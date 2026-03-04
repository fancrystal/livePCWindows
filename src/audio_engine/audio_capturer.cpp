#include "audio_engine/audio_capturer.h"
#include "audio_engine/QtNativeAudioCapturer.h"
#include "common/log.h"

#include <QMediaDevices>
#include <cstdlib>

namespace live_assistant {

// 🔧 调试：强制使用 Qt 采集模式的开关（已禁用）
// 如需测试 Qt 模式，请临时修改此函数返回 true
static bool ShouldUseQtCapture() {
    return false;  // 默认使用 WASAPI
    
    // const char* env = std::getenv("LIVEASSISTANT_USE_QT_AUDIO");
    // return env && (env[0] == '1' || env[0] == 'T' || env[0] == 't' || env[0] == 'Y' || env[0] == 'y');
}

AudioCapturer::AudioCapturer(QObject* parent)
    : QObject(parent)
    , current_device_index_(-1)
    , is_capturing_(false)
    , sample_rate_(0)
    , channels_(0)
    , sample_size_(0)
    , capture_mode_(AudioCaptureMode::FN_STYLE) {  // 默认使用 FN_STYLE
    LOG_INFO("[AudioCapturer] Created, default mode: FN_STYLE");
}

AudioCapturer::~AudioCapturer() {
    stop_capture();
    LOG_INFO("[AudioCapturer] Destroyed");
}

std::vector<AudioDeviceInfo> AudioCapturer::get_wasapi_devices() {
    return WASAPICapturer::EnumerateDevices(true);
}

bool AudioCapturer::start_capture(int sample_rate, int channels, int sample_size) {
    // 停止当前采集
    stop_capture();

    sample_rate_ = sample_rate;
    channels_ = channels;
    sample_size_ = sample_size;

    // 检查是否强制使用 Qt 采集
    bool use_qt = ShouldUseQtCapture();
    if (use_qt) {
        LOG_INFO("[AudioCapturer] 🔧 FORCE QT MODE: LIVEASSISTANT_USE_QT_AUDIO detected!");
    } else {
        LOG_INFO("[AudioCapturer] Using WASAPI (default)");
    }
    
    if (use_qt) {
        LOG_INFO("[AudioCapturer] Using Qt Native Audio capture mode");
        return init_qt_native_capture();
    } else {
        LOG_INFO("[AudioCapturer] Using WASAPI capture mode");
        return init_wasapi_capture();
    }
}

bool AudioCapturer::init_wasapi_capture() {
    LOG_INFO("[AudioCapturer] Initializing WASAPI capture...");

    // 🔧 调试：枚举所有可用的麦克风设备
    {
        auto devices = WASAPICapturer::EnumerateDevices(true);
        LOG_INFO("[AudioCapturer] Found " + std::to_string(devices.size()) + " microphone(s):");
        for (const auto& dev : devices) {
            LOG_INFO("[AudioCapturer]   - " + dev.name + " (id: " + dev.id + ")");
        }
    }

    // 创建 WASAPI 采集器
    wasapi_capturer_ = std::make_unique<WASAPICapturer>();

    // 初始化 WASAPI 采集器
    bool use_default = wasapi_device_id_.empty();
    if (!wasapi_capturer_->Initialize(WASAPISourceType::Input, wasapi_device_id_, use_default)) {
        LOG_ERROR("[AudioCapturer] Failed to initialize WASAPI capturer, falling back to Qt");
        wasapi_capturer_.reset();
        return init_qt_native_capture();  // 回退到 Qt
    }

    // 更新实际参数（WASAPI 使用 32-bit float）
    sample_rate_ = wasapi_capturer_->GetSampleRate();
    channels_ = wasapi_capturer_->GetChannels();
    sample_size_ = wasapi_capturer_->GetBitsPerSample();

    // 设置回调
    wasapi_capturer_->SetCallback([this](const float* data, uint32_t frames,
                                          uint32_t sample_rate, uint32_t channels,
                                          int64_t timestamp) {
        on_wasapi_data(data, frames, sample_rate, channels, timestamp);
    });

    // 启动采集
    if (!wasapi_capturer_->Start()) {
        LOG_ERROR("[AudioCapturer] Failed to start WASAPI capture");
        wasapi_capturer_.reset();
        return false;
    }

    is_capturing_ = true;
    LOG_INFO("[AudioCapturer] WASAPI capture started: " + 
             std::to_string(sample_rate_) + "Hz, " +
             std::to_string(channels_) + " channels, " +
             std::to_string(sample_size_) + " bits");
    return true;
}

bool AudioCapturer::init_qt_native_capture() {
    LOG_INFO("[AudioCapturer] Initializing Qt Native Audio capture...");

    // 创建 Qt 原生采集器
    qt_native_capturer_ = std::make_unique<QtNativeAudioCapturer>();

    // 设置回调
    qt_native_capturer_->SetCallback([this](const float* data, uint32_t frames,
                                            uint32_t sample_rate, uint32_t channels,
                                            int64_t timestamp) {
        on_wasapi_data(data, frames, sample_rate, channels, timestamp);
    });

    // 启动采集
    if (!qt_native_capturer_->Start(sample_rate_, channels_, sample_size_)) {
        LOG_ERROR("[AudioCapturer] Failed to start Qt Native capture");
        qt_native_capturer_.reset();
        return false;
    }

    // 更新实际参数
    sample_rate_ = qt_native_capturer_->GetSampleRate();
    channels_ = qt_native_capturer_->GetChannels();
    sample_size_ = qt_native_capturer_->GetSampleSize();

    is_capturing_ = true;
    LOG_INFO("[AudioCapturer] Qt Native capture started: " + 
             std::to_string(sample_rate_) + "Hz, " +
             std::to_string(channels_) + " channels, " +
             std::to_string(sample_size_) + " bits");
    return true;
}

void AudioCapturer::on_wasapi_data(const float* data, uint32_t frames,
                                    uint32_t sample_rate, uint32_t channels,
                                    int64_t timestamp) {
    // 输出统一为 32-bit float 格式
    
    // 计算数据大小：float 每个样本 4 字节
    size_t data_size = frames * channels * sizeof(float);
    
    // 转换纳秒时间戳为毫秒
    int64_t timestamp_ms = timestamp / 1000000;
    
    // 调试日志
    static int callback_counter = 0;
    callback_counter++;
    if (callback_counter <= 10 || callback_counter % 50 == 0) {
        LOG_INFO("[AudioCapturer] Audio callback: frames=" + std::to_string(frames) +
                 ", sample_rate=" + std::to_string(sample_rate) +
                 ", channels=" + std::to_string(channels) +
                 ", timestamp=" + std::to_string(timestamp_ms) + "ms" +
                 ", data_size=" + std::to_string(data_size) + " bytes");
    }
    
    // 创建 QByteArray
    QByteArray byte_data(reinterpret_cast<const char*>(data), static_cast<int>(data_size));
    
    // 发送信号
    emit data_captured(std::move(byte_data), timestamp_ms);
}

void AudioCapturer::stop_capture() {
    if (!is_capturing_) {
        return;
    }

    // 停止 WASAPI 采集
    if (wasapi_capturer_) {
        wasapi_capturer_->Stop();
        wasapi_capturer_.reset();
    }

    // 停止 Qt Native 采集
    if (qt_native_capturer_) {
        qt_native_capturer_->Stop();
        qt_native_capturer_.reset();
    }

    is_capturing_ = false;
    LOG_INFO("[AudioCapturer] Stopped");
}

QStringList AudioCapturer::get_available_devices() const {
    // 优先使用 WASAPI 设备列表
    return QtNativeAudioCapturer::GetAvailableDevices();
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

void AudioCapturer::set_audio_device(const QAudioDevice& audio_device) {
    if (is_capturing_) {
        stop_capture();
    }

    // TODO: 将设备信息传递给采集器
    Q_UNUSED(audio_device);
}

} // namespace live_assistant
