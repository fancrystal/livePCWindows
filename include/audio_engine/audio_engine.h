#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>

#include <QObject>
#include <QByteArray>

#include "common/error.h"
#include "common/media_clock.h"

namespace live_assistant {

// 前向声明
struct AudioFrame;
class AudioCapturer;

class AudioEngine : public QObject {
    Q_OBJECT

public:
    AudioEngine();
    ~AudioEngine();

    bool initialize(int sample_rate, int channels);
    bool shutdown();

    bool start_capture();
    bool stop_capture();

    struct AudioDeviceInfo {
        std::string id;     // Qt device id (UTF-8)
        std::string name;   // Friendly name (UTF-8)
    };

    std::vector<AudioDeviceInfo> get_available_microphones();
    bool select_microphone(const std::string& mic_id);
    std::string get_selected_microphone_id();

    // Speaker device methods
    std::vector<AudioDeviceInfo> get_available_speakers();
    bool select_speaker(const std::string& speaker_id);
    std::string get_selected_speaker_id();

    bool enable_noise_suppression(bool enable);
    bool enable_echo_cancellation(bool enable);

    std::shared_ptr<AudioFrame> get_audio_frame();

    // Volume control (0.0 to 1.0)
    bool set_microphone_volume(float volume);
    float get_microphone_volume();

    // Mute/unmute
    bool set_microphone_mute(bool mute);
    bool get_microphone_mute();

    // Speaker volume control (system level)
    bool set_speaker_volume(float volume);
    float get_speaker_volume();

    bool set_speaker_mute(bool mute);
    bool get_speaker_mute();

    bool add_audio_source(std::shared_ptr<AudioEngine> source);
    bool remove_audio_source(std::shared_ptr<AudioEngine> source);

    int get_sample_rate() const;
    int get_channels() const;

signals:
    // 发送原始音频数据（用于编码器）- 直接转发 QByteArray，不使用队列
    void audio_data_ready(const QByteArray& data, int64_t timestamp);

public slots:
    // 处理 AudioCapturer 的数据捕获信号
    void on_data_captured(QByteArray data, int64_t timestamp);

private:

    int sample_rate_ = 0;
    int channels_ = 0;
    int sample_size_ = 16;  // 16-bit PCM

    bool is_capturing_ = false;
    std::string selected_microphone_id_;
    std::string selected_speaker_id_;

    bool noise_suppression_enabled_ = false;
    bool echo_cancellation_enabled_ = false;

    // Volume control (protected by state_mutex_)
    float microphone_volume_ = 1.0f;
    bool microphone_muted_ = false;
    float speaker_volume_ = 1.0f;
    bool speaker_muted_ = false;

    // 音频捕获器（从原项目移植）
    std::unique_ptr<AudioCapturer> audio_capturer_;

    std::mutex frame_mutex_;
    std::condition_variable frame_cv_;
    std::queue<std::shared_ptr<AudioFrame>> frame_queue_;

    // Protects state variables that may be accessed from multiple threads
    std::mutex state_mutex_;

    std::vector<std::weak_ptr<AudioEngine>> audio_sources_;
    std::mutex sources_mutex_;
};

struct AudioFrame {
    float* data = nullptr;
    int16_t* raw_data = nullptr;

    MediaTimestamp timestamp;

    int64_t timestamp_ms = 0;

    int sample_rate = 0;
    int channels = 0;
    int samples = 0;

    AudioFrame() = default;
    AudioFrame(int sample_rate, int channels, int samples);
    ~AudioFrame();

    AudioFrame(const AudioFrame&) = delete;
    AudioFrame& operator=(const AudioFrame&) = delete;

    AudioFrame(AudioFrame&& other) noexcept;
    AudioFrame& operator=(AudioFrame&& other) noexcept;

    void convert_raw_to_float();
    void convert_float_to_raw();
};

} // namespace live_assistant
