#include "audio_engine/audio_engine.h"
#include "audio_engine/audio_capturer.h"
#include "common/log.h"
#include "common/media_clock.h"

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
#include <mmsystem.h>     // timeBeginPeriod / timeEndPeriod
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")
#endif

namespace live_assistant {

// 🔧 诊断：保存音频帧到文件
static QFile* g_media_audio_file = nullptr;
static QFile* g_mixed_audio_file = nullptr;
static int g_media_audio_save_count = 0;
static int g_mixed_audio_save_count = 0;

static bool is_filtered_audio_device_name(const QString& device_name) {
    const QString normalized = device_name.trimmed().toLower();
    static const QStringList blocked_patterns = {
        "usb camera audio",
        "virtual",
        "虚拟",
        "todesk"
    };

    for (const QString& pattern : blocked_patterns) {
        if (normalized.contains(pattern)) {
            return true;
        }
    }
    return false;
}

static std::string choose_preferred_audio_device_id(
    const QList<QAudioDevice>& devices,
    const QAudioDevice& system_default_device,
    const char* log_prefix
) {
    const auto is_usable = [](const QAudioDevice& device) {
        return !device.isNull() && !is_filtered_audio_device_name(device.description());
    };

    if (is_usable(system_default_device)) {
        LOG_INFO(std::string(log_prefix) + " using system default device: " +
                 system_default_device.description().toStdString());
        return system_default_device.id().toStdString();
    }

    if (!system_default_device.isNull()) {
        LOG_WARNING(std::string(log_prefix) + " system default device filtered out: " +
                    system_default_device.description().toStdString());
    }

    for (const QAudioDevice& device : devices) {
        if (is_usable(device)) {
            LOG_INFO(std::string(log_prefix) + " falling back to preferred device: " +
                     device.description().toStdString());
            return device.id().toStdString();
        }
    }

    LOG_WARNING(std::string(log_prefix) + " no usable audio devices found after filtering");
    return "default";
}

static std::shared_ptr<AudioFrame> make_frame_from_interleaved_float(
    const QByteArray& data,
    int src_sample_rate,
    int src_channels,
    int dst_sample_rate,
    int dst_channels,
    int64_t timestamp_ms) {
    if (data.isEmpty() || src_channels <= 0 || dst_channels <= 0) {
        return nullptr;
    }

    const int bytes_per_sample = static_cast<int>(sizeof(float));
    const int src_samples = data.size() / (src_channels * bytes_per_sample);
    if (src_samples <= 0) {
        return nullptr;
    }

    auto frame = std::make_shared<AudioFrame>(dst_sample_rate, dst_channels, src_samples);
    if (!frame || !frame->data) {
        return nullptr;
    }

    const float* src = reinterpret_cast<const float*>(data.constData());

    if (src_channels == dst_channels) {
        std::copy_n(src, src_samples * dst_channels, frame->data);
    } else if (src_channels == 1 && dst_channels == 2) {
        for (int i = 0; i < src_samples; ++i) {
            const float sample = src[i];
            frame->data[i * 2] = sample;
            frame->data[i * 2 + 1] = sample;
        }
    } else if (src_channels == 2 && dst_channels == 1) {
        for (int i = 0; i < src_samples; ++i) {
            frame->data[i] = 0.5f * (src[i * 2] + src[i * 2 + 1]);
        }
    } else {
        for (int i = 0; i < src_samples; ++i) {
            for (int ch = 0; ch < dst_channels; ++ch) {
                const int src_ch = (ch < src_channels) ? ch : (src_channels - 1);
                frame->data[i * dst_channels + ch] = src[i * src_channels + src_ch];
            }
        }
    }

    frame->timestamp_ms = timestamp_ms;
    return frame;
}

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

    // Use Qt::DirectConnection so on_data_captured runs immediately in the
    // WASAPI capture thread instead of being queued through the Qt event loop.
    // Without this, there is a 30-50ms startup delay where the mixer can't find
    // frames and fills the audio stream with silence, causing audible A/V desync.
    // on_data_captured is thread-safe: is_capturing_ is atomic, and the queue
    // write is protected by microphone_mutex_.
    connect(audio_capturer_.get(), &AudioCapturer::data_captured,
            this, &AudioEngine::on_data_captured, Qt::DirectConnection);

    connect(audio_capturer_.get(), &AudioCapturer::speaker_data_captured,
            this, &AudioEngine::on_speaker_data_captured, Qt::DirectConnection);

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
        // Pass the WASAPI device ID directly so WASAPICapturer opens the correct
        // device instead of always falling back to the system default.
        audio_capturer_->set_wasapi_device_id(selected_microphone_id_);
        // Also log the friendly name for diagnostics.
        QList<QAudioDevice> devices = QMediaDevices::audioInputs();
        for (const QAudioDevice& device : devices) {
            if (device.id().toStdString() == selected_microphone_id_) {
                LOG_INFO("[AudioEngine] Using selected device: " + device.description().toStdString());
                break;
            }
        }
    }

    // 使用 AudioCapturer 启动捕获
    bool result = audio_capturer_->start_capture(sample_rate_, channels_, 32);  // 使用 Float 32位格式

    if (result) {
        is_capturing_ = true;
        {
            std::lock_guard<std::mutex> lock(timestamp_sync_mutex_);
            capture_clock_epoch_ = std::chrono::steady_clock::now();
            microphone_timestamp_anchor_ms_ = -1;
            speaker_timestamp_anchor_ms_ = -1;
            microphone_clock_anchor_ms_ = 0;
            speaker_clock_anchor_ms_ = 0;
        }
        // 更新实际的采样率和声道数（可能与期望值不同）
        const int capture_sample_rate = audio_capturer_->get_sample_rate();
        const int capture_channels = audio_capturer_->get_channels();

        float volume = get_microphone_volume();
        bool muted = get_microphone_mute();

        LOG_INFO("[AudioEngine] Capture STARTED successfully!");
        LOG_INFO("[AudioEngine]   Format: " + std::to_string(sample_rate_) + "Hz, " +
                 std::to_string(channels_) + " ch, Int16");
        LOG_INFO("[AudioEngine]   Capture source format: " +
                 std::to_string(capture_sample_rate) + "Hz, " +
                 std::to_string(capture_channels) + " ch, Float32");
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

    const int src_sample_rate = audio_capturer_ ? audio_capturer_->get_sample_rate() : sample_rate_;
    const int src_channels = audio_capturer_ ? audio_capturer_->get_channels() : channels_;
    auto frame = make_frame_from_interleaved_float(
        data,
        src_sample_rate,
        src_channels,
        sample_rate_,
        channels_,
        mapCaptureTimestampToEngineClock(timestamp, false));

    if (frame) {
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

    const int src_sample_rate = sample_rate_;
    const int src_channels = channels_;
    auto frame = make_frame_from_interleaved_float(
        data,
        src_sample_rate > 0 ? src_sample_rate : sample_rate_,
        src_channels > 0 ? src_channels : channels_,
        sample_rate_,
        channels_,
        mapCaptureTimestampToEngineClock(timestamp, true));

    if (frame) {
        pushFrameToQueue(speaker_queue_, speaker_mutex_, frame);
    }
}

std::vector<AudioEngine::AudioDeviceInfo> AudioEngine::get_available_microphones() {
    std::vector<AudioDeviceInfo> devices;

    QList<QAudioDevice> audio_devices = QMediaDevices::audioInputs();
    LOG_INFO("[AudioEngine] Enumerating " + std::to_string(audio_devices.size()) + " audio input devices");

    for (int i = 0; i < audio_devices.size(); ++i) {
        const QAudioDevice& device = audio_devices[i];
        if (is_filtered_audio_device_name(device.description())) {
            LOG_INFO("[AudioEngine]   [filtered] " + device.description().toStdString() +
                     " (id: " + device.id().toStdString() + ")");
            continue;
        }
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

std::string AudioEngine::refresh_microphone_to_system_default() {
    // Always use whatever Windows reports as the current default input device.
    // No filtering or fallback — the user controls their default device in the
    // system sound settings; we should respect that choice unconditionally.
    const QAudioDevice default_device = QMediaDevices::defaultAudioInput();
    std::string selected_id;
    if (!default_device.isNull()) {
        selected_id = default_device.id().toStdString();
        LOG_INFO("[AudioEngine][MicSync] Using system default microphone: " +
                 default_device.description().toStdString());
    } else {
        selected_id = "default";
        LOG_WARNING("[AudioEngine][MicSync] No default microphone found, using 'default'");
    }
    select_microphone(selected_id);
    return selected_id;
}

std::vector<AudioEngine::AudioDeviceInfo> AudioEngine::get_available_speakers() {
    std::vector<AudioDeviceInfo> devices;

    QList<QAudioDevice> audio_devices = QMediaDevices::audioOutputs();
    for (const QAudioDevice& device : audio_devices) {
        if (is_filtered_audio_device_name(device.description())) {
            LOG_INFO("[AudioEngine]   [filtered speaker] " + device.description().toStdString() +
                     " (id: " + device.id().toStdString() + ")");
            continue;
        }
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

std::string AudioEngine::refresh_speaker_to_system_default() {
    // Always use whatever Windows reports as the current default output device.
    const QAudioDevice default_device = QMediaDevices::defaultAudioOutput();
    std::string selected_id;
    if (!default_device.isNull()) {
        selected_id = default_device.id().toStdString();
        LOG_INFO("[AudioEngine][SpeakerSync] Using system default speaker: " +
                 default_device.description().toStdString());
    } else {
        selected_id = "default";
        LOG_WARNING("[AudioEngine][SpeakerSync] No default speaker found, using 'default'");
    }
    select_speaker(selected_id);
    return selected_id;
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
    // Only control the software-level mix flag. Do NOT touch the system audio
    // endpoint volume — that would change the user's system speaker volume.
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    std::lock_guard<std::mutex> lock(state_mutex_);
    speaker_volume_ = volume;
    LOG_INFO("[AudioEngine] Speaker mix volume set to " + std::to_string(static_cast<int>(volume * 100)) + "%");
    return true;
}

float AudioEngine::get_system_microphone_volume() {
#ifdef _WIN32
    IMMDeviceEnumerator* pEnumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr)) return -1.0f;

    IMMDevice* pDevice = nullptr;
    if (!selected_microphone_id_.empty() && selected_microphone_id_ != "default") {
        std::wstring w_id(selected_microphone_id_.begin(), selected_microphone_id_.end());
        hr = pEnumerator->GetDevice(w_id.c_str(), &pDevice);
    } else {
        hr = pEnumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &pDevice);
    }
    pEnumerator->Release();
    if (FAILED(hr) || !pDevice) return -1.0f;

    IAudioEndpointVolume* pEndpointVolume = nullptr;
    hr = pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                           nullptr, (void**)&pEndpointVolume);
    pDevice->Release();
    if (FAILED(hr) || !pEndpointVolume) return -1.0f;

    float vol = -1.0f;
    hr = pEndpointVolume->GetMasterVolumeLevelScalar(&vol);
    pEndpointVolume->Release();
    return SUCCEEDED(hr) ? vol : -1.0f;
#else
    return -1.0f;
#endif
}

float AudioEngine::get_system_speaker_volume() {
#ifdef _WIN32
    IMMDeviceEnumerator* pEnumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr)) return -1.0f;

    IMMDevice* pDevice = nullptr;
    if (!selected_speaker_id_.empty() && selected_speaker_id_ != "default") {
        std::wstring w_id(selected_speaker_id_.begin(), selected_speaker_id_.end());
        hr = pEnumerator->GetDevice(w_id.c_str(), &pDevice);
    } else {
        hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    }
    pEnumerator->Release();
    if (FAILED(hr) || !pDevice) return -1.0f;

    IAudioEndpointVolume* pEndpointVolume = nullptr;
    hr = pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                           nullptr, (void**)&pEndpointVolume);
    pDevice->Release();
    if (FAILED(hr) || !pEndpointVolume) return -1.0f;

    float vol = -1.0f;
    hr = pEndpointVolume->GetMasterVolumeLevelScalar(&vol);
    pEndpointVolume->Release();
    return SUCCEEDED(hr) ? vol : -1.0f;
#else
    return -1.0f;
#endif
}

float AudioEngine::get_speaker_volume() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return speaker_volume_;
}

bool AudioEngine::set_speaker_mute(bool mute) {
    // Only control the software-level mix flag. Do NOT touch the system audio
    // endpoint volume — modifying IAudioEndpointVolume::SetMute would silence
    // the user's system speakers, which is never the intent here.
    std::lock_guard<std::mutex> lock(state_mutex_);
    speaker_muted_ = mute;
    LOG_INFO("[AudioEngine] Speaker mix " + std::string(mute ? "MUTED" : "UNMUTED"));
    return true;
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

    // Timestamp is intentionally NOT derived from source frames here.
    // It will be overridden by MediaClock::get_elapsed_time_us() in mixThreadFunc
    // so that audio and video share the same reference frame.
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
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    LOG_INFO("[AudioMixer] Mix thread started (timer resolution set to 1ms)");

    // WASAPI currently delivers ~10 ms chunks (480 samples at 48 kHz). If we
    // always pace the mixer as if it emitted 1024-sample blocks, the source
    // queues grow forever and start dropping frames.
    auto thread_epoch = std::chrono::steady_clock::now();
    int64_t next_target_us = 0;

    while (mix_thread_running_.load()) {
        AudioMixMode current_mode = getMixMode();
        QList<AudioSourceType> active_sources = getActiveSourcesByMode(current_mode);
        QList<std::shared_ptr<AudioFrame>> sources;
        sources_types_.clear();

        static int mix_debug_count = 0;
        if (mix_debug_count < 10) {
            LOG_INFO("[AudioMixer] Mix mode: " + std::to_string(static_cast<int>(current_mode)) +
                     ", active sources: " + std::to_string(active_sources.size()));
            mix_debug_count++;
        }

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
                sources.append(frame);
                sources_types_.append(type);
            } else {
                static int get_frame_fail_count = 0;
                if (get_frame_fail_count < 10) {
                    LOG_INFO("[AudioMixer] Failed to get frame for source type: " +
                             std::to_string(static_cast<int>(type)));
                    get_frame_fail_count++;
                }
            }
        }

        std::shared_ptr<AudioFrame> mixed_frame = nullptr;
        int emitted_samples = 0;
        if (!sources.isEmpty()) {
            mixed_frame = mixMultipleSources(sources, sources_types_);
            if (mixed_frame) {
                emitted_samples = mixed_frame->samples;
            }
        } else {
            int fallback_sample_rate = sample_rate_ > 0 ? sample_rate_ : 48000;
            int fallback_channels = channels_ > 0 ? channels_ : 2;
            int fallback_samples = fallback_sample_rate / 100;  // ~10 ms
            if (fallback_samples <= 0) {
                fallback_samples = 480;
            }

            mixed_frame = std::make_shared<AudioFrame>(
                fallback_sample_rate, fallback_channels, fallback_samples);
            if (mixed_frame && mixed_frame->data) {
                std::fill_n(
                    mixed_frame->data,
                    mixed_frame->samples * mixed_frame->channels,
                    0.0f);
                emitted_samples = mixed_frame->samples;
            }
        }

        if (mixed_frame && mixed_frame->data) {
            // Pass steady_clock absolute microseconds at emit time so the bridge
            // can reconstruct an accurate PTS even when the slot runs later in the
            // main thread (QueuedConnection).  The bridge subtracts its own
            // streaming_start_steady_us_ to get elapsed time in the bridge's domain.
            int64_t emit_steady_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            int data_size = mixed_frame->samples * mixed_frame->channels * sizeof(float);
            QByteArray data(reinterpret_cast<const char*>(mixed_frame->data), data_size);
            emit audio_data_ready(data, emit_steady_us);
        }

        int pacing_sample_rate = sample_rate_ > 0 ? sample_rate_ : 48000;
        if (emitted_samples <= 0) {
            emitted_samples = pacing_sample_rate / 100;  // ~10 ms
        }
        if (emitted_samples <= 0) {
            emitted_samples = 480;
        }

        next_target_us += (static_cast<int64_t>(emitted_samples) * 1000000LL) / pacing_sample_rate;

        auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - thread_epoch).count();
        int64_t sleep_us = next_target_us - now_us;
        if (sleep_us > 1000) {
            std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
        }
    }

    LOG_INFO("[AudioMixer] Mix thread stopped");
#ifdef _WIN32
    timeEndPeriod(1);
#endif
}

std::shared_ptr<AudioFrame> AudioEngine::getFrameFromQueue(
    std::queue<std::shared_ptr<AudioFrame>>& queue,
    std::mutex& mutex
) {
    std::lock_guard<std::mutex> lock(mutex);

    if (queue.empty()) {
        return nullptr;
    }

    // Take the front (oldest) frame. The mix-thread pacing already ensures the
    // queue stays near-empty during normal streaming.  Unbounded growth is
    // prevented by the MAX_QUEUE_SIZE cap in pushFrameToQueue.
    // Draining to the newest frame is intentionally avoided because it causes
    // non-monotonic source timestamps when two sources (mic + speaker) are
    // mixed, leading to repeated "Large timestamp regression" warnings in the
    // AAC encoder.  Timestamps are now derived from MediaClock in mixThreadFunc,
    // so source frame timestamps are no longer critical.
    auto frame = queue.front();
    queue.pop();
    return frame;
}

int64_t AudioEngine::mapCaptureTimestampToEngineClock(
    int64_t source_timestamp_ms,
    bool is_speaker_source
) {
    if (source_timestamp_ms < 0) {
        source_timestamp_ms = 0;
    }

    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(timestamp_sync_mutex_);

    if (capture_clock_epoch_ == std::chrono::steady_clock::time_point{}) {
        capture_clock_epoch_ = now;
    }

    int64_t& source_anchor_ms = is_speaker_source
        ? speaker_timestamp_anchor_ms_
        : microphone_timestamp_anchor_ms_;
    int64_t& clock_anchor_ms = is_speaker_source
        ? speaker_clock_anchor_ms_
        : microphone_clock_anchor_ms_;

    if (source_anchor_ms < 0) {
        source_anchor_ms = source_timestamp_ms;
        clock_anchor_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - capture_clock_epoch_).count();
    }

    return clock_anchor_ms + (source_timestamp_ms - source_anchor_ms);
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
        if (drop_count <= 5 || drop_count % 500 == 0) {
            LOG_WARNING("[AudioMixer] Queue full, dropping oldest frame (total dropped: " +
                      std::to_string(drop_count) + ")");
        }
    }

    queue.push(frame);
}

} // namespace live_assistant
