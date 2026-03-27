#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <functional>

#include <QObject>
#include <QByteArray>
#include <QMap>
#include <QMutex>

#include "common/error.h"
#include "common/media_clock.h"

namespace live_assistant {

// 前向声明
struct AudioFrame;
class AudioCapturer;

/**
 * @brief 音频源类型枚举
 */
enum class AudioSourceType {
    MICROPHONE,   // 麦克风
    SPEAKER,      // 扬声器/系统音频
    MEDIA,        // 插播媒体音频
    CUSTOM        // 自定义音频源（预留扩展）
};

/**
 * @brief 混音模式枚举
 */
enum class AudioMixMode {
    MIC_ONLY,              // 只用麦克风（默认）
    SPEAKER_ONLY,          // 只用扬声器
    MEDIA_ONLY,            // 只用插播音频
    MIC_SPEAKER,           // 麦克风 + 扬声器
    MIC_MEDIA,             // 麦克风 + 插播
    SPEAKER_MEDIA,         // 扬声器 + 插播
    MIC_SPEAKER_MEDIA      // 麦克风 + 扬声器 + 插播（全混音）
};

/**
 * @brief 音频源配置
 */
struct AudioSourceConfig {
    AudioSourceType type;
    float volume = 1.0f;           // 音量 (0.0 - 1.0)
    bool muted = false;            // 是否静音
    bool enabled = true;           // 是否启用
};

class AudioEngine : public QObject {
    Q_OBJECT

public:
    AudioEngine();
    ~AudioEngine();

    bool initialize(int sample_rate, int channels);
    bool shutdown();

    bool start_capture();
    bool stop_capture();

    // 检查是否正在采集
    bool is_capturing() const { return is_capturing_; }

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

    // 🔧 新增：获取单独的麦克风帧
    std::shared_ptr<AudioFrame> get_microphone_frame();

    // Volume control (0.0 to 1.0)
    bool set_microphone_volume(float volume);
    float get_microphone_volume();

    // Mute/unmute
    bool set_microphone_mute(bool mute);
    bool get_microphone_mute();

    // 🔧 降噪功能
    bool set_noise_suppression(bool enabled);
    bool get_noise_suppression() const;
    bool set_noise_suppression_level(float level);  // 0.0 - 1.0
    float get_noise_suppression_level() const;

    // Speaker volume control (system level)
    bool set_speaker_volume(float volume);
    float get_speaker_volume();

    bool set_speaker_mute(bool mute);
    bool get_speaker_mute();
    
    // 获取音频采集器（用于控制扬声器采集）
    AudioCapturer* get_audio_capturer() const { return audio_capturer_.get(); }

    bool add_audio_source(std::shared_ptr<AudioEngine> source);
    bool remove_audio_source(std::shared_ptr<AudioEngine> source);

    // ========== 插播媒体控制 ==========
    bool set_media_volume(float volume);
    float get_media_volume() const;
    bool set_media_mute(bool mute);
    bool get_media_mute() const;

    // ========== 混音模式控制 ==========
    void setMixMode(AudioMixMode mode);
    AudioMixMode getMixMode() const;

    // ========== 媒体音频推送（重构）==========
    void pushMediaFrame(std::shared_ptr<AudioFrame> frame);

    // ========== 预留扩展接口 ==========

    /**
     * @brief 注册自定义音频源处理器（扩展接口）
     * @param sourceId 音频源ID
     * @param callback 音频数据回调函数
     */
    void registerAudioSourceCallback(const QString& sourceId,
        std::function<std::shared_ptr<AudioFrame>()> callback);

    /**
     * @brief 注销自定义音频源
     */
    void unregisterAudioSource(const QString& sourceId);

    // ========== 音频源管理 ==========

    /**
     * @brief 添加音频源（预留扩展接口）
     * @param sourceId 音频源ID
     * @param type 音频源类型
     * @return 是否成功
     */
    bool addAudioSource(const QString& sourceId, AudioSourceType type);

    /**
     * @brief 移除音频源
     * @param sourceId 音频源ID
     */
    void removeAudioSource(const QString& sourceId);

    /**
     * @brief 获取音频源配置
     */
    AudioSourceConfig getSourceConfig(const QString& sourceId) const;

    /**
     * @brief 设置音频源音量
     * @param sourceId 音频源ID
     * @param volume 音量 (0.0 - 1.0)
     */
    void setSourceVolume(const QString& sourceId, float volume);

    /**
     * @brief 设置音频源静音
     * @param sourceId 音频源ID
     * @param muted 是否静音
     */
    void setSourceMute(const QString& sourceId, bool muted);

    /**
     * @brief 设置音频源启用/禁用
     * @param sourceId 音频源ID
     * @param enabled 是否启用
     */
    void setSourceEnabled(const QString& sourceId, bool enabled);

    int get_sample_rate() const;
    int get_channels() const;

signals:
    // 发送原始音频数据（用于编码器）- 直接转发 QByteArray，不使用队列
    void audio_data_ready(const QByteArray& data, int64_t timestamp);

    // 音频源状态变化信号
    void sourceAdded(const QString& sourceId, AudioSourceType type);
    void sourceRemoved(const QString& sourceId);
    void sourceVolumeChanged(const QString& sourceId, float volume);
    void sourceMuteChanged(const QString& sourceId, bool muted);

    // 混音模式变化信号
    void mixModeChanged(AudioMixMode mode);

public slots:
    // 处理 AudioCapturer 的数据捕获信号（麦克风）
    void on_data_captured(QByteArray data, int64_t timestamp);
    
    // 处理 AudioCapturer 的数据捕获信号（扬声器/桌面音频）
    void on_speaker_data_captured(QByteArray data, int64_t timestamp);

private:

    // ========== 混音实现 ==========

    /**
     * @brief 混音处理（核心方法）
     * @param sources 输入音频源列表
     * @param source_types 音频源类型列表
     * @return 混音后的音频帧
     */
    std::shared_ptr<AudioFrame> mixMultipleSources(
        const QList<std::shared_ptr<AudioFrame>>& sources,
        const QList<AudioSourceType>& source_types
    );

    /**
     * @brief 获取指定类型的音频帧
     */
    std::shared_ptr<AudioFrame> getAudioFrameByType(AudioSourceType type);

    /**
     * @brief 根据混音模式获取需要混音的源
     */
    QList<AudioSourceType> getActiveSourcesByMode(AudioMixMode mode);

    // ========== 数据成员 ==========

    // 当前混音模式（默认：只麦克风）
    AudioMixMode mix_mode_ = AudioMixMode::MIC_ONLY;

    // 各音频源配置
    QMap<QString, AudioSourceConfig> source_configs_;

    // 麦克风（已有）
    float microphone_volume_ = 1.0f;
    bool microphone_muted_ = false;

    // 扬声器（新增）
    float speaker_volume_ = 1.0f;
    bool speaker_muted_ = false;

    // 插播媒体（新增）
    float media_volume_ = 0.7f;
    bool media_muted_ = false;

    // 音频捕获器（从原项目移植）
    std::unique_ptr<AudioCapturer> audio_capturer_;

    // 插播媒体源（新增）
    std::weak_ptr<AudioFrame> media_source_frame_;

    // 自定义音频源回调（扩展接口）
    QMap<QString, std::function<std::shared_ptr<AudioFrame>()>> custom_source_callbacks_;
    std::mutex custom_sources_mutex_;

    int sample_rate_ = 0;
    int channels_ = 0;
    int sample_size_ = 16;  // 16-bit PCM

    bool is_capturing_ = false;
    std::string selected_microphone_id_;
    std::string selected_speaker_id_;

    bool noise_suppression_enabled_ = false;
    bool echo_cancellation_enabled_ = false;

    // Volume control (protected by state_mutex_)
    float microphone_volume_protected_ = 1.0f;
    bool microphone_muted_protected_ = false;

    // 🔧 降噪功能
    float noise_suppression_level_ = 0.5f;
    int16_t noise_gate_threshold_ = 500;
    float speaker_volume_protected_ = 1.0f;
    bool speaker_muted_protected_ = false;

    std::mutex frame_mutex_;
    std::condition_variable frame_cv_;
    std::queue<std::shared_ptr<AudioFrame>> frame_queue_;

    // ========== 独立混音线程（重构）==========
    std::thread mix_thread_;
    std::atomic<bool> mix_thread_running_{false};
    static const int MIX_THREAD_INTERVAL_MS = 21;  // 48kHz / 1024 samples ≈ 21.33ms

    // 各音频源独立队列（FIFO）
    std::queue<std::shared_ptr<AudioFrame>> microphone_queue_;
    std::queue<std::shared_ptr<AudioFrame>> media_queue_;
    std::queue<std::shared_ptr<AudioFrame>> speaker_queue_;
    
    // 队列互斥锁
    std::mutex microphone_mutex_;
    std::mutex media_mutex_;
    std::mutex speaker_mutex_;

    // 队列大小限制：20帧约420ms，为混音线程提供足够缓冲避免爆音
    static const size_t MAX_QUEUE_SIZE = 20;

    // 当前混音的源类型列表（用于音量控制）
    QList<AudioSourceType> sources_types_;

    // 混音线程主函数
    void mixThreadFunc();

    // 从队列获取帧（FIFO）
    std::shared_ptr<AudioFrame> getFrameFromQueue(
        std::queue<std::shared_ptr<AudioFrame>>& queue,
        std::mutex& mutex
    );

    // 将帧推入队列
    void pushFrameToQueue(
        std::queue<std::shared_ptr<AudioFrame>>& queue,
        std::mutex& mutex,
        std::shared_ptr<AudioFrame> frame
    );

    // Protects state variables that may be accessed from multiple threads
    mutable std::mutex state_mutex_;

    std::vector<std::weak_ptr<AudioEngine>> audio_sources_;
    std::mutex sources_mutex_;
};

struct AudioFrame {
    float* data = nullptr;          // float 格式音频数据 (交错格式)
                                     // 范围: -1.0 到 1.0

    // 🔧 统一使用毫秒时间戳 (timestamp_ms)
    int64_t timestamp_ms = 0;

    int sample_rate = 0;
    int channels = 0;
    int samples = 0;                // 每个通道的样本数

    AudioFrame() = default;
    AudioFrame(int sample_rate, int channels, int samples);
    ~AudioFrame();

    AudioFrame(const AudioFrame&) = delete;
    AudioFrame& operator=(const AudioFrame&) = delete;

    AudioFrame(AudioFrame&& other) noexcept;
    AudioFrame& operator=(AudioFrame&& other) noexcept;

    // 获取数据大小（字节）
    size_t data_size() const { return samples * channels * sizeof(float); }
};

} // namespace live_assistant
