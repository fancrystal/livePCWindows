#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <atomic>

#include "common/error.h"
#include "common/media_clock.h"

// Windows音频接口的前向声明
struct IMMDeviceEnumerator;
struct IMMDevice;
struct IAudioClient;
struct IAudioCaptureClient;

namespace live_assistant {

// 前向声明
struct AudioFrame;

// AudioEngine配置结构体
struct AudioEngineConfig {
    int sample_rate = 44100;
    int channels = 2;
    std::string microphone_id;
    bool enable_noise_suppression = false;
    bool enable_echo_cancellation = false;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    bool initialize(int sample_rate, int channels);
    bool shutdown();

    bool start_capture();
    bool stop_capture();
    
    // 获取可用音频设备
    std::vector<std::string> get_available_microphones();
    bool select_microphone(const std::string& mic_id);
    
    // 基本音频处理
    bool enable_noise_suppression(bool enable);
    bool enable_echo_cancellation(bool enable);
    
    // 获取处理后的音频帧
    std::shared_ptr<AudioFrame> get_audio_frame();
    
    // 音频混音
    bool add_audio_source(std::shared_ptr<AudioEngine> source);
    bool remove_audio_source(std::shared_ptr<AudioEngine> source);
    
    // 获取音频参数
    int get_sample_rate() const;
    int get_channels() const;
    
private:
    // WASAPI音频捕获线程函数
    void capture_thread_func();
    
    // WASAPI初始化
    ErrorCode initialize_wasapi();
    
    // WASAPI清理
    void cleanup_wasapi();
    
    int sample_rate_ = 48000; // 根据要求默认使用48kHz
    int channels_ = 2; // 默认使用立体声
    
    bool is_capturing_ = false;
    std::string selected_microphone_;
    
    bool noise_suppression_enabled_ = false;
    bool echo_cancellation_enabled_ = false;
    
    // WASAPI相关成员
    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* audio_device_ = nullptr;
    IAudioClient* audio_client_ = nullptr;
    IAudioCaptureClient* capture_client_ = nullptr;
    void* format_ = nullptr;
    
    // 捕获线程和同步
    std::thread capture_thread_;
    std::atomic<bool> stop_capture_flag_ = false;
    
    // 捕获缓冲区和帧队列
    std::mutex frame_mutex_;
    std::condition_variable frame_cv_;
    std::queue<std::shared_ptr<AudioFrame>> frame_queue_;
    
    // 用于混音的音频源 - 使用weak_ptr避免循环引用
    std::vector<std::weak_ptr<AudioEngine>> audio_sources_;
    std::mutex sources_mutex_;
};

// 简单的音频帧结构
struct AudioFrame {
    // 浮点格式的数据（用于处理）
    float* data = nullptr;
    
    // s16格式的原始数据（用于捕获）
    int16_t* raw_data = nullptr;
    
    // 媒体时间戳（微秒）
    MediaTimestamp timestamp;
    
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
    
    // 将s16原始数据转换为浮点
    void convert_raw_to_float();
    
    // 将浮点数据转换为s16原始数据
    void convert_float_to_raw();
};

} // namespace live_assistant
