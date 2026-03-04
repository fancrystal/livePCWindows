#include "audio_engine/audio_engine.h"
#include "audio_engine/audio_capturer.h"
#include "common/log.h"

#include <QMediaDevices>
#include <QAudioDevice>
#include <QMutex>
#include <cmath>

// Windows Speaker volume control
#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#pragma comment(lib, "ole32.lib")
#endif

namespace live_assistant {

AudioEngine::AudioEngine() : QObject(nullptr) {
    // 创建音频捕获器
    audio_capturer_ = std::make_unique<AudioCapturer>(this);

    // 连接信号
    connect(audio_capturer_.get(), &AudioCapturer::data_captured,
            this, &AudioEngine::on_data_captured);

    LOG_INFO("[AudioEngine] Created (using AudioCapturer)");
}

AudioEngine::~AudioEngine() {
    shutdown();
    LOG_INFO("[AudioEngine] Destroyed");
}

bool AudioEngine::initialize(int sample_rate, int channels) {
    // 保存期望的配置
    sample_rate_ = sample_rate;
    channels_ = channels;
    LOG_INFO("[AudioEngine] Initialized: desired_sample_rate=" + std::to_string(sample_rate) +
             ", desired_channels=" + std::to_string(channels));
    return true;
}

bool AudioEngine::shutdown() {
    stop_capture();

    // 清空帧队列
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        int queue_size = frame_queue_.size();
        while (!frame_queue_.empty()) {
            frame_queue_.pop();
        }
        if (queue_size > 0) {
            LOG_INFO("[AudioEngine] Shutdown: cleared " + std::to_string(queue_size) + " pending frames");
        }
    }

    LOG_INFO("[AudioEngine] Shutdown complete");
    return true;
}

bool AudioEngine::start_capture() {
    if (is_capturing_) {
        LOG_WARNING("[AudioEngine] Capture already running");
        return true;
    }

    LOG_INFO("[AudioEngine] Starting audio capture...");

    // 设置音频设备（如果已选择）
    if (!selected_microphone_id_.empty() && selected_microphone_id_ != "default") {
        QList<QAudioDevice> devices = QMediaDevices::audioInputs();
        for (const QAudioDevice& device : devices) {
            if (device.id().toStdString() == selected_microphone_id_) {
                audio_capturer_->set_audio_device(device);
                LOG_INFO("[AudioEngine] Using selected device: " + device.description().toStdString());
                break;
            }
        }
    }

    // 使用 AudioCapturer 启动捕获
    bool result = audio_capturer_->start_capture(sample_rate_, channels_, 32);  // 使用 Float 32位格式

    if (result) {
        is_capturing_ = true;
        // 更新实际的采样率和声道数（可能与期望值不同）
        sample_rate_ = audio_capturer_->get_sample_rate();
        channels_ = audio_capturer_->get_channels();

        float volume = get_microphone_volume();
        bool muted = get_microphone_mute();

        LOG_INFO("[AudioEngine] Capture STARTED successfully!");
        LOG_INFO("[AudioEngine]   Format: " + std::to_string(sample_rate_) + "Hz, " +
                 std::to_string(channels_) + " ch, Int16");
        LOG_INFO("[AudioEngine]   Volume: " + std::to_string(volume * 100) + "%, Muted: " + (muted ? "YES" : "NO"));
    }

    return result;
}

bool AudioEngine::stop_capture() {
    if (!is_capturing_) {
        LOG_INFO("[AudioEngine] Capture not running, nothing to stop");
        return true;
    }

    LOG_INFO("[AudioEngine] Stopping audio capture...");

    audio_capturer_->stop_capture();
    is_capturing_ = false;

    LOG_INFO("[AudioEngine] Capture STOPPED");
    return true;
}

void AudioEngine::on_data_captured(QByteArray data, int64_t timestamp) {
    if (!is_capturing_ || data.isEmpty()) {
        return;
    }

    // 🔧 诊断：打印原始数据的第一个样本值
    static int dump_counter = 0;
    dump_counter++;
    if (dump_counter <= 3) {
        const float* raw_data = reinterpret_cast<const float*>(data.constData());
        float first_sample = raw_data[0];
        float second_sample = raw_data[1];
        float fifth_sample = (data.size() >= 20) ? raw_data[5] : 0;
        LOG_INFO("[AudioEngine] RAW data dump #" + std::to_string(dump_counter) +
                 ": data_size=" + std::to_string(data.size()) +
                 ", first_sample=" + std::to_string(first_sample) +
                 ", second=" + std::to_string(second_sample) +
                 ", fifth=" + std::to_string(fifth_sample));
    }

    // 数据格式：32-bit Float (从 WASAPI 传来)
    // WASAPI 输出的是 float 格式，每个样本 4 字节
    int bytes_per_sample = sizeof(float);
    int total_samples = data.size() / (channels_ * bytes_per_sample);

    // 创建音频帧（float 格式）
    auto frame = std::make_shared<AudioFrame>(sample_rate_, channels_, total_samples);

    if (frame->data) {
        // 应用音量控制和静音处理
        float volume = microphone_volume_;
        bool muted = microphone_muted_;

        // 直接使用 float 格式，不转换
        const float* src_data = reinterpret_cast<const float*>(data.constData());
        int total = total_samples * channels_;

        for (int i = 0; i < total; ++i) {
            float float_sample = src_data[i];
            
            // 应用音量
            if (!muted) {
                float_sample *= volume;
            } else {
                float_sample = 0.0f;
            }
            
            // 限制范围
            if (float_sample > 1.0f) float_sample = 1.0f;
            if (float_sample < -1.0f) float_sample = -1.0f;
            
            frame->data[i] = float_sample;
        }

        // 设置时间戳
        frame->timestamp_ms = timestamp;
    }

    // 发送信号通知编码器有新数据（直接传递 QByteArray）
    emit audio_data_ready(data, timestamp);

    // 将帧放入队列（保留用于其他可能的用途）
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        frame_queue_.push(frame);
    }
    frame_cv_.notify_one();

    // 诊断日志（简化版）
    static int frame_count = 0;
    static int64_t total_samples_processed = 0;
    static int silent_frame_count = 0;

    frame_count++;
    total_samples_processed += (int64_t)total_samples;

    // 计算音量统计（使用 float 格式）
    double rms = 0.0;
    double peak = 0.0;
    if (frame->data) {
        double sum_squares = 0.0;
        int total = frame->samples * frame->channels;
        for (int i = 0; i < total; ++i) {
            double sample = static_cast<double>(frame->data[i]);
            sum_squares += sample * sample;
            double abs_sample = std::abs(sample);
            if (abs_sample > peak) peak = abs_sample;
        }
        rms = std::sqrt(sum_squares / total);
    }

    bool is_silent = (rms < 0.001);
    if (is_silent) {
        silent_frame_count++;
    }

    // 每100帧打印一次统计
    if (frame_count % 100 == 0) {
        double silent_percent = (silent_frame_count * 100.0) / frame_count;
        LOG_INFO("[AudioEngine] Stats: frames=" + std::to_string(frame_count) +
                 ", samples=" + std::to_string(total_samples_processed) +
                 ", RMS=" + std::to_string(static_cast<int>(rms)) +
                 ", peak=" + std::to_string(static_cast<int>(peak)) +
                 ", silent=" + std::to_string(static_cast<int>(silent_percent)) + "%");
    }
}

std::vector<AudioEngine::AudioDeviceInfo> AudioEngine::get_available_microphones() {
    std::vector<AudioDeviceInfo> devices;

    QList<QAudioDevice> audio_devices = QMediaDevices::audioInputs();
    LOG_INFO("[AudioEngine] Enumerating " + std::to_string(audio_devices.size()) + " audio input devices");

    for (int i = 0; i < audio_devices.size(); ++i) {
        const QAudioDevice& device = audio_devices[i];
        AudioDeviceInfo info;
        info.id = device.id().toStdString();
        info.name = device.description().toStdString();
        devices.push_back(info);
        LOG_INFO("  [" + std::to_string(i) + "] " + info.name + " (id: " + info.id + ")");
    }

    return devices;
}

bool AudioEngine::select_microphone(const std::string& mic_id) {
    if (mic_id.empty()) {
        LOG_ERROR("[AudioEngine] ERROR: Invalid microphone ID (empty string)");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        selected_microphone_id_ = mic_id;
    }
    LOG_INFO("[AudioEngine] Selected microphone ID: " + mic_id);
    return true;
}

std::string AudioEngine::get_selected_microphone_id() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return selected_microphone_id_;
}

std::vector<AudioEngine::AudioDeviceInfo> AudioEngine::get_available_speakers() {
    std::vector<AudioDeviceInfo> devices;

    QList<QAudioDevice> audio_devices = QMediaDevices::audioOutputs();
    for (const QAudioDevice& device : audio_devices) {
        AudioDeviceInfo info;
        info.id = device.id().toStdString();
        info.name = device.description().toStdString();
        devices.push_back(info);
    }

    return devices;
}

bool AudioEngine::select_speaker(const std::string& speaker_id) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    selected_speaker_id_ = speaker_id;
    LOG_INFO("[AudioEngine] Selected speaker ID: " + speaker_id);
    return true;
}

std::string AudioEngine::get_selected_speaker_id() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return selected_speaker_id_;
}

bool AudioEngine::enable_noise_suppression(bool enable) {
    noise_suppression_enabled_ = enable;
    LOG_INFO("[AudioEngine] Noise suppression " + std::string(enable ? "enabled" : "disabled"));
    return true;
}

bool AudioEngine::enable_echo_cancellation(bool enable) {
    echo_cancellation_enabled_ = enable;
    LOG_INFO("[AudioEngine] Echo cancellation " + std::string(enable ? "enabled" : "disabled"));
    return true;
}

std::shared_ptr<AudioFrame> AudioEngine::get_audio_frame() {
    // 🔧 修复：恢复原来的简单逻辑，不在内部混音
    // 混音应该在更上层处理，或者使用单独的混音器
    return get_microphone_frame();
}

// 获取单独的麦克风帧（不经过混音）
std::shared_ptr<AudioFrame> AudioEngine::get_microphone_frame() {
    std::shared_ptr<AudioFrame> frame = nullptr;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (!frame_queue_.empty()) {
            frame = frame_queue_.front();
            frame_queue_.pop();
        }
    }

    // 诊断日志
    static int get_frame_count = 0;
    static int null_frame_count = 0;
    get_frame_count++;

    if (frame) {
        bool all_zero = true;
        if (frame->data) {
            int total = frame->samples * frame->channels;
            for (int i = 0; i < total; ++i) {
                if (frame->data[i] != 0.0f) { all_zero = false; break; }
            }
        }

        if (get_frame_count % 50 == 0) {
            LOG_INFO("[AudioEngine] get_audio_frame: samples=" + std::to_string(frame->samples) +
                     ", channels=" + std::to_string(frame->channels) +
                     ", all_zero=" + (all_zero ? "YES" : "NO") +
                     ", queue_depth=" + std::to_string([&]() {
                         std::lock_guard<std::mutex> lock(frame_mutex_);
                         return frame_queue_.size();
                     }()));
        }
    } else {
        null_frame_count++;
        if (null_frame_count % 100 == 0) {
            LOG_WARNING("[AudioEngine] get_audio_frame: returned null " +
                       std::to_string(null_frame_count) + " times (audio queue empty)");
        }
    }

    return frame;
}

bool AudioEngine::set_microphone_volume(float volume) {
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    std::lock_guard<std::mutex> lock(state_mutex_);
    microphone_volume_ = volume;
    LOG_INFO("[AudioEngine] Microphone volume set to " + std::to_string(static_cast<int>(volume * 100)) + "%");
    return true;
}

float AudioEngine::get_microphone_volume() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return microphone_volume_;
}

bool AudioEngine::set_microphone_mute(bool mute) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    microphone_muted_ = mute;
    LOG_INFO("[AudioEngine] Microphone " + std::string(mute ? "MUTED" : "UNMUTED"));
    return true;
}

bool AudioEngine::get_microphone_mute() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return microphone_muted_;
}

bool AudioEngine::set_noise_suppression(bool enabled) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    noise_suppression_enabled_ = enabled;
    LOG_INFO("[AudioEngine] Noise suppression " + std::string(enabled ? "enabled" : "disabled"));
    return true;
}

bool AudioEngine::get_noise_suppression() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return noise_suppression_enabled_;
}

bool AudioEngine::set_noise_suppression_level(float level) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    noise_suppression_level_ = level;
    noise_gate_threshold_ = static_cast<int16_t>(noise_suppression_level_ * 2000);
    LOG_INFO("[AudioEngine] Noise suppression level: " + std::to_string(noise_suppression_level_));
    return true;
}

float AudioEngine::get_noise_suppression_level() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return noise_suppression_level_;
}

bool AudioEngine::set_speaker_volume(float volume) {
#ifdef _WIN32
    HRESULT hr = S_OK;
    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDevice* pDevice = nullptr;
    IAudioEndpointVolume* pEndpointVolume = nullptr;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (SUCCEEDED(hr)) {
        if (!selected_speaker_id_.empty() && selected_speaker_id_ != "default") {
            std::wstring w_id(selected_speaker_id_.begin(), selected_speaker_id_.end());
            hr = pEnumerator->GetDevice(w_id.c_str(), &pDevice);
        } else {
            hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
        }

        if (SUCCEEDED(hr)) {
            hr = pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                                   nullptr, (void**)&pEndpointVolume);
            if (SUCCEEDED(hr)) {
                hr = pEndpointVolume->SetMasterVolumeLevelScalar(volume, nullptr);
                pEndpointVolume->Release();
            }
            pDevice->Release();
        }
        pEnumerator->Release();
    }

    if (SUCCEEDED(hr)) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        speaker_volume_ = volume;
        LOG_INFO("[AudioEngine] Speaker volume set to " + std::to_string(static_cast<int>(volume * 100)) + "%");
        return true;
    }
    LOG_ERROR("[AudioEngine] Failed to set speaker volume");
    return false;
#else
    std::lock_guard<std::mutex> lock(state_mutex_);
    speaker_volume_ = volume;
    LOG_INFO("[AudioEngine] Speaker volume set to " + std::to_string(static_cast<int>(volume * 100)) + "%");
    return true;
#endif
}

float AudioEngine::get_speaker_volume() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return speaker_volume_;
}

bool AudioEngine::set_speaker_mute(bool mute) {
#ifdef _WIN32
    HRESULT hr = S_OK;
    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDevice* pDevice = nullptr;
    IAudioEndpointVolume* pEndpointVolume = nullptr;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (SUCCEEDED(hr)) {
        if (!selected_speaker_id_.empty() && selected_speaker_id_ != "default") {
            std::wstring w_id(selected_speaker_id_.begin(), selected_speaker_id_.end());
            hr = pEnumerator->GetDevice(w_id.c_str(), &pDevice);
        } else {
            hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
        }

        if (SUCCEEDED(hr)) {
            hr = pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                                   nullptr, (void**)&pEndpointVolume);
            if (SUCCEEDED(hr)) {
                pEndpointVolume->SetMute(mute, nullptr);
                pEndpointVolume->Release();
            }
            pDevice->Release();
        }
        pEnumerator->Release();
    }

    if (SUCCEEDED(hr)) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        speaker_muted_ = mute;
        LOG_INFO("[AudioEngine] Speaker " + std::string(mute ? "MUTED" : "UNMUTED"));
        return true;
    }
    LOG_ERROR("[AudioEngine] Failed to set speaker mute");
    return false;
#else
    std::lock_guard<std::mutex> lock(state_mutex_);
    speaker_muted_ = mute;
    LOG_INFO("[AudioEngine] Speaker " + std::string(mute ? "MUTED" : "UNMUTED"));
    return true;
#endif
}

bool AudioEngine::get_speaker_mute() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return speaker_muted_;
}

bool AudioEngine::add_audio_source(std::shared_ptr<AudioEngine> source) {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    audio_sources_.push_back(source);
    return true;
}

bool AudioEngine::remove_audio_source(std::shared_ptr<AudioEngine> source) {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    auto it = std::remove_if(audio_sources_.begin(), audio_sources_.end(),
        [&source](const std::weak_ptr<AudioEngine>& weak) {
            return weak.lock() == source;
        });
    audio_sources_.erase(it, audio_sources_.end());
    return true;
}

int AudioEngine::get_sample_rate() const {
    return sample_rate_;
}

int AudioEngine::get_channels() const {
    return channels_;
}

//=============================================================================
// 音频源管理
//=============================================================================

bool AudioEngine::addAudioSource(const QString& sourceId, AudioSourceType type) {
    QMutexLocker locker(&state_mutex_);

    if (source_configs_.contains(sourceId)) {
        LOG_WARNING("[AudioEngine] Source already exists: " + sourceId.toStdString());
        return false;
    }

    AudioSourceConfig config;
    config.type = type;
    config.volume = 1.0f;
    config.muted = false;
    config.enabled = true;
    source_configs_[sourceId] = config;

    emit sourceAdded(sourceId, type);
    LOG_INFO("[AudioEngine] Added audio source: " + sourceId.toStdString() +
             ", type: " + std::to_string(static_cast<int>(type)));
    return true;
}

void AudioEngine::removeAudioSource(const QString& sourceId) {
    QMutexLocker locker(&state_mutex_);

    if (!source_configs_.contains(sourceId)) {
        return;
    }

    source_configs_.remove(sourceId);
    emit sourceRemoved(sourceId);

    LOG_INFO("[AudioEngine] Removed audio source: " + sourceId.toStdString());
}

AudioSourceConfig AudioEngine::getSourceConfig(const QString& sourceId) const {
    std::lock_guard<std::mutex> locker(state_mutex_);
    return source_configs_.value(sourceId, AudioSourceConfig());
}

void AudioEngine::setSourceVolume(const QString& sourceId, float volume) {
    QMutexLocker locker(&state_mutex_);

    volume = (std::max)(0.0f, (std::min)(1.0f, volume));
    if (source_configs_.contains(sourceId)) {
        source_configs_[sourceId].volume = volume;
        emit sourceVolumeChanged(sourceId, volume);
        LOG_INFO("[AudioEngine] Source volume changed: " + sourceId.toStdString() +
                 ", volume: " + std::to_string(static_cast<int>(volume * 100)) + "%");
    }
}

void AudioEngine::setSourceMute(const QString& sourceId, bool muted) {
    QMutexLocker locker(&state_mutex_);

    if (source_configs_.contains(sourceId)) {
        source_configs_[sourceId].muted = muted;
        emit sourceMuteChanged(sourceId, muted);
        LOG_INFO("[AudioEngine] Source mute changed: " + sourceId.toStdString() +
                 ", muted: " + std::string(muted ? "true" : "false"));
    }
}

void AudioEngine::setSourceEnabled(const QString& sourceId, bool enabled) {
    QMutexLocker locker(&state_mutex_);

    if (source_configs_.contains(sourceId)) {
        source_configs_[sourceId].enabled = enabled;
        LOG_INFO("[AudioEngine] Source enabled changed: " + sourceId.toStdString() +
                 ", enabled: " + std::string(enabled ? "true" : "false"));
    }
}

//=============================================================================
// 插播媒体控制
//=============================================================================

bool AudioEngine::set_media_volume(float volume) {
    volume = (std::max)(0.0f, (std::min)(1.0f, volume));
    std::lock_guard<std::mutex> lock(state_mutex_);
    media_volume_ = volume;
    LOG_INFO("[AudioEngine] Media volume set to " + std::to_string(static_cast<int>(volume * 100)) + "%");
    return true;
}

float AudioEngine::get_media_volume() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return media_volume_;
}

bool AudioEngine::set_media_mute(bool mute) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    media_muted_ = mute;
    LOG_INFO("[AudioEngine] Media " + std::string(mute ? "MUTED" : "UNMUTED"));
    return true;
}

bool AudioEngine::get_media_mute() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return media_muted_;
}

//=============================================================================
// 混音模式控制
//=============================================================================

void AudioEngine::setMixMode(AudioMixMode mode) {
    if (mix_mode_ != mode) {
        mix_mode_ = mode;
        emit mixModeChanged(mode);
        LOG_INFO("[AudioEngine] Mix mode changed to: " + std::to_string(static_cast<int>(mode)));
    }
}

AudioMixMode AudioEngine::getMixMode() const {
    return mix_mode_;
}

//=============================================================================
// 自定义音频源扩展
//=============================================================================

void AudioEngine::registerAudioSourceCallback(const QString& sourceId,
    std::function<std::shared_ptr<AudioFrame>()> callback) {
    std::lock_guard<std::mutex> lock(custom_sources_mutex_);
    custom_source_callbacks_[sourceId] = callback;
    LOG_INFO("[AudioEngine] Registered audio source callback: " + sourceId.toStdString());
}

void AudioEngine::unregisterAudioSource(const QString& sourceId) {
    std::lock_guard<std::mutex> lock(custom_sources_mutex_);
    custom_source_callbacks_.remove(sourceId);
    LOG_INFO("[AudioEngine] Unregistered audio source: " + sourceId.toStdString());
}

//=============================================================================
// 混音实现
//=============================================================================

std::shared_ptr<AudioFrame> AudioEngine::mixMultipleSources(
    const QList<std::shared_ptr<AudioFrame>>& sources) {

    if (sources.isEmpty()) {
        return nullptr;
    }

    // 获取第一个帧的参数作为输出参数
    std::shared_ptr<AudioFrame> firstFrame = sources.first();
    int out_sample_rate = firstFrame->sample_rate;
    int out_channels = firstFrame->channels;
    int out_samples = firstFrame->samples;

    // 创建输出帧
    auto output = std::make_shared<AudioFrame>(out_sample_rate, out_channels, out_samples);

    if (!output->data) {
        return nullptr;
    }

    // 初始化输出为0
    std::fill_n(output->data, out_samples * out_channels, 0.0f);

    // 遍历所有源并混音
    for (const auto& frame : sources) {
        if (!frame || !frame->data) continue;

        // 确保参数匹配
        if (frame->sample_rate != out_sample_rate ||
            frame->channels != out_channels ||
            frame->samples != out_samples) {
            // 参数不匹配时跳过（实际项目中应该重采样）
            continue;
        }

        // 应用音量
        float volume = 1.0f; // 默认音量
        // 可以根据源类型设置不同音量

        for (int i = 0; i < out_samples * out_channels; ++i) {
            float mixed = output->data[i] + frame->data[i] * volume;
            // 限制在有效范围内
            if (mixed > 1.0f) mixed = 1.0f;
            if (mixed < -1.0f) mixed = -1.0f;
            output->data[i] = mixed;
        }
    }

    return output;
}

std::shared_ptr<AudioFrame> AudioEngine::getAudioFrameByType(AudioSourceType type) {
    switch (type) {
    case AudioSourceType::MICROPHONE:
        return get_microphone_frame();
    case AudioSourceType::MEDIA: {
        // 从自定义回调获取媒体音频
        std::lock_guard<std::mutex> lock(custom_sources_mutex_);
        for (auto it = custom_source_callbacks_.begin(); it != custom_source_callbacks_.end(); ++it) {
            if (it.value()) {
                auto frame = it.value()();
                if (frame) {
                    return frame;
                }
            }
        }
        return nullptr;
    }
    case AudioSourceType::SPEAKER:
    case AudioSourceType::CUSTOM:
    default:
        return nullptr;
    }
}

QList<AudioSourceType> AudioEngine::getActiveSourcesByMode(AudioMixMode mode) {
    QList<AudioSourceType> sources;

    switch (mode) {
    case AudioMixMode::MIC_ONLY:
        sources.append(AudioSourceType::MICROPHONE);
        break;
    case AudioMixMode::SPEAKER_ONLY:
        sources.append(AudioSourceType::SPEAKER);
        break;
    case AudioMixMode::MEDIA_ONLY:
        sources.append(AudioSourceType::MEDIA);
        break;
    case AudioMixMode::MIC_SPEAKER:
        sources.append(AudioSourceType::MICROPHONE);
        sources.append(AudioSourceType::SPEAKER);
        break;
    case AudioMixMode::MIC_MEDIA:
        sources.append(AudioSourceType::MICROPHONE);
        sources.append(AudioSourceType::MEDIA);
        break;
    case AudioMixMode::SPEAKER_MEDIA:
        sources.append(AudioSourceType::SPEAKER);
        sources.append(AudioSourceType::MEDIA);
        break;
    case AudioMixMode::MIC_SPEAKER_MEDIA:
        sources.append(AudioSourceType::MICROPHONE);
        sources.append(AudioSourceType::SPEAKER);
        sources.append(AudioSourceType::MEDIA);
        break;
    }

    return sources;
}

//=============================================================================
// AudioFrame 实现
//=============================================================================

AudioFrame::AudioFrame(int sample_rate, int channels, int samples)
    : sample_rate(sample_rate), channels(channels), samples(samples) {
    size_t float_size = samples * channels * sizeof(float);
    data = new (std::nothrow) float[samples * channels];
    if (data) memset(data, 0, float_size);
}

AudioFrame::~AudioFrame() {
    delete[] data;
}

AudioFrame::AudioFrame(AudioFrame&& other) noexcept
    : data(other.data), sample_rate(other.sample_rate),
      channels(other.channels), samples(other.samples) {
    other.data = nullptr;
}

AudioFrame& AudioFrame::operator=(AudioFrame&& other) noexcept {
    if (this != &other) {
        delete[] data;
        data = other.data;
        sample_rate = other.sample_rate;
        channels = other.channels;
        samples = other.samples;
        other.data = nullptr;
    }
    return *this;
}

} // namespace live_assistant
