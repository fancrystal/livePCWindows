#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <deque>

namespace live_assistant {

struct AudioDeviceInfo {
    std::string id;
    std::string name;
};

enum class WASAPISourceType {
    Input,           // 麦克风输入采集
    DeviceOutput,    // 设备输出采集（桌面音频）
};

class WASAPICapturer {
public:
    // 固定帧大小（参照 OBS 的 AUDIO_OUTPUT_FRAMES）
    static constexpr uint32_t AUDIO_FRAMES_PER_CALLBACK = 1024;

    using AudioDataCallback = std::function<void(const float* data, uint32_t frames, 
                                                  uint32_t sample_rate, uint32_t channels,
                                                  int64_t timestamp)>;

    WASAPICapturer();
    ~WASAPICapturer();

    // 禁止拷贝
    WASAPICapturer(const WASAPICapturer&) = delete;
    WASAPICapturer& operator=(const WASAPICapturer&) = delete;

    // 获取可用的音频输入设备列表
    static std::vector<AudioDeviceInfo> EnumerateDevices(bool input = true);

    // 初始化采集器
    bool Initialize(WASAPISourceType type, const std::string& device_id = "", 
                    bool use_default_device = true);

    // 设置音频数据回调
    void SetCallback(AudioDataCallback callback);

    // 开始采集
    bool Start();

    // 停止采集
    void Stop();

    // 获取当前配置
    uint32_t GetSampleRate() const { return sample_rate_; }
    uint32_t GetChannels() const { return channels_; }
    uint32_t GetBitsPerSample() const { return 32; }  // WASAPI 始终是 float
    bool IsCapturing() const { return is_capturing_; }

private:
    // 初始化设备
    Microsoft::WRL::ComPtr<IMMDevice> InitDevice(const std::string& device_id, bool use_default_device);

    // 初始化音频客户端
    Microsoft::WRL::ComPtr<IAudioClient> InitClient(IMMDevice* device);

    // 初始化捕获客户端
    Microsoft::WRL::ComPtr<IAudioCaptureClient> InitCapture(IAudioClient* client, HANDLE event_handle);

    // 处理捕获数据（从 WASAPI 接收，累积到缓冲区）
    bool ProcessCaptureData();

    // 从累积缓冲区输出固定 1024 帧
    void OutputFixedFrames();

    // 采集线程函数
    static DWORD WINAPI CaptureThread(LPVOID param);

    // 获取设备名称
    static std::string GetDeviceName(IMMDevice* device);

private:
    // COM 接口
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<IMMDevice> device_;
    Microsoft::WRL::ComPtr<IAudioClient> audio_client_;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_client_;

    // 配置
    WASAPISourceType source_type_ = WASAPISourceType::Input;
    std::string device_id_;
    std::string device_name_;
    bool use_default_device_ = true;

    // 音频格式
    uint32_t sample_rate_ = 48000;
    uint32_t channels_ = 2;
    uint32_t bits_per_sample_ = 32;
    bool is_float_format_ = true;  // true = IEEE_FLOAT, false = PCM

    // 线程控制
    std::atomic<bool> is_capturing_{false};
    HANDLE capture_thread_ = nullptr;
    HANDLE stop_event_ = nullptr;
    HANDLE receive_event_ = nullptr;

    // 回调
    AudioDataCallback callback_;
    std::mutex callback_mutex_;

    // 静音缓冲区
    std::vector<uint8_t> silence_buffer_;

    // 累积缓冲区（参照 OBS）
    // 存储交错格式的 float 音频数据
    std::deque<float> audio_accumulator_;
    std::mutex accumulator_mutex_;
    
    // 累计样本数（用于计算 PTS）
    uint64_t total_samples_ = 0;
};

} // namespace live_assistant
