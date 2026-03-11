#include "audio_engine/audio_engine.h"
#include "audio_engine/audio_capturer.h"
#include "common/log.h"

#include <QMediaDevices>
#include <QAudioDevice>
#include <QMutex>
#include <QFile>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <thread>

// Windows Speaker volume control
#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#pragma comment(lib, "ole32.lib")
#endif

namespace live_assistant {

// 🔧 诊断：保存音频帧到文件
static QFile* g_media_audio_file = nullptr;
static QFile* g_mixed_audio_file = nullptr;
static int g_media_audio_save_count = 0;
static int g_mixed_audio_save_count = 0;

static void save_audio_to_file(const float* data, int samples, int channels, QFile*& file, const char* filename, int& save_count, int max_save) {
    if (!file) {
        file = new QFile(filename);
        if (file->open(QIODevice::WriteOnly)) {
            LOG_INFO("[AudioDiag] Opened audio file: " + std::string(filename));
        } else {
            LOG_ERROR("[AudioDiag] Failed to open audio file: " + std::string(filename));
            delete file;
            file = nullptr;
            return;
        }
    }
    
    if (save_count < max_save && file->isOpen()) {
        // 保存为原始 float 数据
        file->write(reinterpret_cast<const char*>(data), samples * channels * sizeof(float));
        save_count++;
        if (save_count <= 3) {
            float first = data[0];
            float second = data[1];
            LOG_INFO("[AudioDiag] Saved audio to " + std::string(filename) + 
                     ": samples=" + std::to_string(samples) + 
                     ", first_sample=" + std::to_string(first));
        }
    }
}

AudioEngine::AudioEngine() : QObject(nullptr) {
    // 创建音频捕获器
    audio_capturer_ = std::make_unique<AudioCapturer>(this);

    // 连接信号（麦克风数据）
    connect(audio_capturer_.get(), &AudioCapturer::data_captured,
            this, &AudioEngine::on_data_captured);
    
    // 连接信号（扬声器/桌面音频数据）
    connect(audio_capturer_.get(), &AudioCapturer::speaker_data_captured,
            this, &AudioEngine::on_speaker_data_captured);

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

        // 🔧 修复：混音线程在音频引擎初始化时就启动，不依赖是否推流
        // 参照OBS的实现方式，混音线程独立运行
        if (!mix_thread_running_.load()) {
            mix_thread_running_.store(true);
            mix_thread_ = std::thread(&AudioEngine::mixThreadFunc, this);
            LOG_INFO("[AudioEngine] Mix thread started");
        }
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

    // 停止混音线程
    mix_thread_running_.store(false);
    if (mix_thread_.joinable()) {
        mix_thread_.join();
        LOG_INFO("[AudioEngine] Mix thread stopped");
    }

    LOG_INFO("[AudioEngine] Capture STOPPED");
    return true;
}

void AudioEngine::on_data_captured(QByteArray data, int64_t timestamp) {
    if (!is_capturing_ || data.isEmpty()) {
        return;
    }

    // 数据格式：32-bit Float (从 WASAPI 传来)
    int bytes_per_sample = sizeof(float);
    int total_samples = data.size() / (channels_ * bytes_per_sample);

    // 创建音频帧
    auto frame = std::make_shared<AudioFrame>(sample_rate_, channels_, total_samples);

    if (frame->data) {
        // 复制数据
        const float* src_data = reinterpret_cast<const float*>(data.constData());
        int total = total_samples * channels_;

        for (int i = 0; i < total; ++i) {
            float float_sample = src_data[i];
            frame->data[i] = float_sample;
        }

        // 设置时间戳
        frame->timestamp_ms = timestamp;

        // 推入麦克风队列
        pushFrameToQueue(microphone_queue_, microphone_mutex_, frame);
    }
}

void AudioEngine::on_speaker_data_captured(QByteArray data, int64_t timestamp) {
    // 检查是否启用了扬声器采集
    if (!audio_capturer_ || !audio_capturer_->is_speaker_capture_enabled()) {
        return;
    }
    
    if (data.isEmpty()) {
        return;
    }

    // 数据格式：32-bit Float (从 WASAPI 传来)
    int bytes_per_sample = sizeof(float);
    int total_samples = data.size() / (channels_ * bytes_per_sample);

    // 创建音频帧
    auto frame = std::make_shared<AudioFrame>(sample_rate_, channels_, total_samples);

    if (frame->data) {
        // 复制数据
        const float* src_data = reinterpret_cast<const float*>(data.constData());
        int total = total_samples * channels_;

        for (int i = 0; i < total; ++i) {
            float float_sample = src_data[i];
            frame->data[i] = float_sample;
        }

        // 设置时间戳
        frame->timestamp_ms = timestamp;

        // 推入扬声器队列
        pushFrameToQueue(speaker_queue_, speaker_mutex_, frame);
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

void AudioEngine::pushMediaFrame(std::shared_ptr<AudioFrame> frame) {
    // 混音线程未启动时（推流尚未开始），丢弃媒体帧
    // 避免在推流前积压大量帧导致 media_queue_ 溢出、后续音频卡顿
    if (!mix_thread_running_.load()) {
        return;
    }
    pushFrameToQueue(media_queue_, media_mutex_, frame);
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
    const QList<std::shared_ptr<AudioFrame>>& sources,
    const QList<AudioSourceType>& source_types
) {

    // 🔧 严格检查：sources 列表
    if (sources.isEmpty()) {
        LOG_ERROR("[AudioMixer] ERROR: sources list is empty!");
        return nullptr;
    }

    // 获取第一个帧的参数作为输出参数
    std::shared_ptr<AudioFrame> firstFrame = sources.first();
    
    // 🔧 严格检查：firstFrame 有效性
    if (!firstFrame) {
        LOG_ERROR("[AudioMixer] ERROR: firstFrame is nullptr!");
        return nullptr;
    }
    
    if (!firstFrame->data) {
        LOG_ERROR("[AudioMixer] ERROR: firstFrame->data is null!");
        return nullptr;
    }
    
    int out_sample_rate = firstFrame->sample_rate;
    int out_channels = firstFrame->channels;
    int out_samples = firstFrame->samples;
    
    // 创建输出帧
    auto output = std::make_shared<AudioFrame>(out_sample_rate, out_channels, out_samples);

    if (!output->data) {
        LOG_ERROR("[AudioMixer] ERROR: Failed to allocate output frame data!");
        return nullptr;
    }

    // 初始化输出为 0
    std::fill_n(output->data, out_samples * out_channels, 0.0f);
    
    // 遍历所有源并混音
    for (int i = 0; i < sources.size(); ++i) {
        const auto& frame = sources[i];
        AudioSourceType source_type = source_types[i];
        
        if (!frame || !frame->data) continue;

        // 🔧 修复：处理采样数不匹配的情况
        // 使用两个帧中较小的样本数
        int mix_samples = (out_samples < frame->samples) ? out_samples : frame->samples;
        if (frame->sample_rate != out_sample_rate ||
            frame->channels != out_channels) {
            // 采样率或声道数不匹配时跳过
            LOG_ERROR("[AudioMixer] Skipping frame: rate/channels mismatch");
            continue;
        }

        // 🔧 根据源类型应用正确的音量
        float volume = 1.0f;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            switch (source_type) {
                case AudioSourceType::MICROPHONE:
                    volume = microphone_muted_ ? 0.0f : microphone_volume_;
                    break;
                case AudioSourceType::MEDIA:
                    volume = media_muted_ ? 0.0f : media_volume_;
                    break;
                case AudioSourceType::SPEAKER:
                    volume = speaker_muted_ ? 0.0f : speaker_volume_;
                    break;
                default:
                    volume = 1.0f;
                    break;
            }
        }

        for (int j = 0; j < mix_samples * out_channels; ++j) {
            float mixed = output->data[j] + frame->data[j] * volume;
            // 限制在有效范围内
            if (mixed > 1.0f) mixed = 1.0f;
            if (mixed < -1.0f) mixed = -1.0f;
            output->data[j] = mixed;
        }
    }

    return output;
}

std::shared_ptr<AudioFrame> AudioEngine::getAudioFrameByType(AudioSourceType type) {
    switch (type) {
    case AudioSourceType::MICROPHONE:
        return get_microphone_frame();
    case AudioSourceType::MEDIA: {
        // 从媒体队列获取帧
        return getFrameFromQueue(media_queue_, media_mutex_);
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

//=============================================================================
// 独立混音线程实现（重构）
//=============================================================================

void AudioEngine::mixThreadFunc() {
    LOG_INFO("[AudioMixer] Mix thread started");

    while (mix_thread_running_.load()) {
        auto start_time = std::chrono::steady_clock::now();

        // 获取当前混音模式
        AudioMixMode current_mode = getMixMode();

        // 根据混音模式获取需要混音的源
        QList<AudioSourceType> active_sources = getActiveSourcesByMode(current_mode);

        // 准备混音源列表
        QList<std::shared_ptr<AudioFrame>> sources;
        sources_types_.clear();  // 清空之前的源类型列表

        // 🔧 诊断：打印混音模式和活动源
        static int mix_debug_count = 0;
        if (mix_debug_count < 10) {
            LOG_INFO("[AudioMixer] Mix mode: " + std::to_string(static_cast<int>(current_mode)) +
                     ", active sources: " + std::to_string(active_sources.size()));
            mix_debug_count++;
        }

        // 获取各源的音频帧
        for (AudioSourceType type : active_sources) {
            std::shared_ptr<AudioFrame> frame = nullptr;

            switch (type) {
                case AudioSourceType::MICROPHONE:
                    frame = getFrameFromQueue(microphone_queue_, microphone_mutex_);
                    break;
                case AudioSourceType::MEDIA:
                    frame = getFrameFromQueue(media_queue_, media_mutex_);
                    break;
                case AudioSourceType::SPEAKER:
                    frame = getFrameFromQueue(speaker_queue_, speaker_mutex_);
                    break;
                default:
                    break;
            }

            if (frame) {
                // 为帧添加来源类型标记（通过混音模式推断）
                // 这里我们直接使用 type 来确定音量
                sources.append(frame);
                sources_types_.append(type);
            } else {
                // 🔧 诊断：打印获取帧失败
                static int get_frame_fail_count = 0;
                if (get_frame_fail_count < 10) {
                    LOG_INFO("[AudioMixer] Failed to get frame for source type: " + 
                             std::to_string(static_cast<int>(type)));
                    get_frame_fail_count++;
                }
            }
        }

        // 执行混音
        std::shared_ptr<AudioFrame> mixed_frame = nullptr;
        if (!sources.isEmpty()) {
            mixed_frame = mixMultipleSources(sources, sources_types_);
        } else {
            // 🔧 修复：所有源都为空，生成静音帧（带时间戳）
            mixed_frame = std::make_shared<AudioFrame>(48000, 2, 1024);
            if (mixed_frame && mixed_frame->data) {
                std::fill_n(mixed_frame->data, 1024 * 2, 0.0f);
                // 🔧 时间戳对齐：使用系统时间
                int64_t current_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                mixed_frame->timestamp_ms = current_time_ms;
            }
        }

        // 发送混音结果
        if (mixed_frame && mixed_frame->data) {
            // 转换为 QByteArray
            int data_size = mixed_frame->samples * mixed_frame->channels * sizeof(float);
            QByteArray data(reinterpret_cast<const char*>(mixed_frame->data), data_size);

            // 发送信号
            emit audio_data_ready(data, mixed_frame->timestamp_ms);
        }

        // 计算休眠时间
        auto end_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

        if (elapsed < MIX_THREAD_INTERVAL_MS) {
            std::this_thread::sleep_for(std::chrono::milliseconds(MIX_THREAD_INTERVAL_MS - elapsed));
        }
    }

    LOG_INFO("[AudioMixer] Mix thread stopped");
}

std::shared_ptr<AudioFrame> AudioEngine::getFrameFromQueue(
    std::queue<std::shared_ptr<AudioFrame>>& queue,
    std::mutex& mutex
) {
    std::lock_guard<std::mutex> lock(mutex);

    if (queue.empty()) {
        return nullptr;
    }

    auto frame = queue.front();
    queue.pop();
    return frame;
}

void AudioEngine::pushFrameToQueue(
    std::queue<std::shared_ptr<AudioFrame>>& queue,
    std::mutex& mutex,
    std::shared_ptr<AudioFrame> frame
) {
    if (!frame) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex);

    // 限制队列大小
    if (queue.size() >= MAX_QUEUE_SIZE) {
        // 队列满，丢弃最旧的帧
        queue.pop();
        static int drop_count = 0;
        drop_count++;
        if (drop_count <= 10) {
            LOG_WARNING("[AudioMixer] Queue full, dropping oldest frame (total dropped: " + 
                      std::to_string(drop_count) + ")");
        }
    }

    queue.push(frame);
}

} // namespace live_assistant
