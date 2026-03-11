#pragma once

#include <string>
#include <memory>
#include <vector>
#include <map>
#include <mutex>
#include <queue>
#include <functional>
#include <QByteArray>

namespace live_assistant {

/**
 * @brief 音频输入源配置
 */
struct AudioInputConfig {
    std::string id;           // 输入源ID
    std::string name;         // 输入源名称
    float volume = 1.0f;      // 音量 (0.0 - 1.0)
    bool muted = false;       // 是否静音
    int sample_rate = 48000;  // 采样率
    int channels = 2;         // 声道数
};

/**
 * @brief 音频帧数据
 */
struct AudioFrameData {
    QByteArray data;          // PCM数据 (int16格式)
    int64_t timestamp_ms = 0; // 时间戳
    int sample_rate = 48000;  // 采样率
    int channels = 2;         // 声道数
    int samples = 0;          // 采样数
};

/**
 * @brief 音频混音器
 * 
 * 功能：
 * - 支持多路音频输入（麦克风、插播视频、扬声器、应用声音等）
 * - 实时音频混合
 * - 音量控制和静音
 * - 输出统一格式的音频帧
 */
class AudioMixer {
public:
    using MixedAudioCallback = std::function<void(const AudioFrameData&)>;

    AudioMixer();
    ~AudioMixer();

    // 初始化混音器
    bool initialize(int sample_rate = 48000, int channels = 2);
    void shutdown();

    // 添加/移除音频输入源
    bool add_input(const AudioInputConfig& config);
    void remove_input(const std::string& input_id);
    bool has_input(const std::string& input_id) const;

    // 设置输入源音量
    void set_input_volume(const std::string& input_id, float volume);
    float get_input_volume(const std::string& input_id) const;

    // 设置输入源静音
    void set_input_muted(const std::string& input_id, bool muted);
    bool is_input_muted(const std::string& input_id) const;

    // 提交音频数据到指定输入源
    bool submit_audio(const std::string& input_id, const QByteArray& data, int64_t timestamp_ms);

    // 设置混音后的音频回调
    void set_mixed_audio_callback(MixedAudioCallback callback);

    // 获取混音器配置
    int get_sample_rate() const { return sample_rate_; }
    int get_channels() const { return channels_; }

    // 获取所有输入源列表
    std::vector<AudioInputConfig> get_inputs() const;

    // 🔧 降噪功能
    void set_noise_suppression_enabled(bool enabled);
    bool is_noise_suppression_enabled() const;
    void set_noise_suppression_level(float level);  // 0.0 - 1.0

private:
    // 混音线程函数
    void mixer_thread_func();

    // 执行混音
    void do_mixing();

    // 混合两路音频数据
    QByteArray mix_audio_data(const std::vector<QByteArray>& inputs);

    // 限制音量防止溢出
    int16_t clamp_sample(int32_t sample);

private:
    bool initialized_ = false;
    bool running_ = false;

    int sample_rate_ = 48000;
    int channels_ = 2;
    int frame_samples_ = 1024;  // AAC帧大小

    // 输入源配置
    std::map<std::string, AudioInputConfig> input_configs_;
    mutable std::mutex config_mutex_;

    // 输入音频队列
    struct InputQueue {
        std::queue<AudioFrameData> queue;
        std::mutex mutex;
    };
    std::map<std::string, std::shared_ptr<InputQueue>> input_queues_;
    mutable std::mutex queues_mutex_;

    // 混音线程
    std::thread mixer_thread_;

    // 混音后音频回调
    MixedAudioCallback mixed_audio_callback_;
    std::mutex callback_mutex_;

    // 混音时间戳
    int64_t mix_timestamp_ms_ = 0;

    // 🔧 降噪功能
    bool noise_suppression_enabled_ = false;
    float noise_suppression_level_ = 0.5f;
    int16_t noise_gate_threshold_ = 500;  // 噪声门限阈值
};

} // namespace live_assistant
