#pragma once

#include <cstdint>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <memory>

namespace live_assistant {

// 音频输出回调类型（照搬 OBS）
using AudioOutputCallback = std::function<void(
    uint64_t timestamp_ns,      // 时间戳（纳秒）
    const float* const* data,   // 音频数据（平面格式）
    uint32_t frames,            // 帧数
    uint32_t channels           // 通道数
)>;

// 音频输出配置
struct AudioOutputConfig {
    uint32_t sample_rate = 48000;
    uint32_t channels = 2;
    uint32_t frames_per_buffer = 1024;  // 每缓冲区帧数（AUDIO_OUTPUT_FRAMES）
};

// 音频输入（音频源注册）
struct AudioInput {
    std::string source_id;
    AudioOutputCallback callback;
};

/**
 * @brief 音频输出系统（照搬 OBS audio-io.c）
 * 
 * 核心设计：
 * 1. 独立音频线程，固定周期运行
 * 2. 收集所有音频源并混音
 * 3. 输出固定大小的音频块
 */
class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    // 初始化
    bool initialize(const AudioOutputConfig& config);
    
    // 关闭
    void shutdown();

    // 启动音频线程
    bool start();
    
    // 停止音频线程
    void stop();

    // 是否正在运行
    bool is_running() const;

    // 注册音频输入源（麦克风、媒体等）
    void register_input(const std::string& source_id, AudioOutputCallback callback);
    
    // 注销音频输入源
    void unregister_input(const std::string& source_id);

    // 设置输出回调（编码器注册这个）
    void set_output_callback(AudioOutputCallback callback);

    // 获取配置
    const AudioOutputConfig& get_config() const { return config_; }

private:
    // 音频线程主循环（照搬 audio_thread）
    void audio_thread();

    // 输入和输出处理（照搬 input_and_output）
    void input_and_output(uint64_t audio_time_ns, uint64_t prev_time_ns);

    // 执行混音
    void do_mixing(float** mix_buffers, uint32_t frames);

private:
    AudioOutputConfig config_;
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    
    std::thread audio_thread_;
    
    // 输入源列表
    std::mutex inputs_mutex_;
    std::vector<AudioInput> inputs_;
    
    // 输出回调
    std::mutex output_mutex_;
    AudioOutputCallback output_callback_;
    
    // 混音缓冲区
    std::vector<std::vector<float>> mix_buffers_;
    
    // 线程同步
    uint64_t start_time_ns_ = 0;
    uint64_t total_samples_ = 0;
};

} // namespace live_assistant
