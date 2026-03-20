#include "audio_engine/wasapi_capturer.h"
#include "common/log.h"

#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <chrono>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Avrt.lib")

namespace live_assistant {

// 缓冲区时间（100纳秒单位）- 5秒
static constexpr REFERENCE_TIME BUFFER_TIME_100NS = 5 * 10000000;

WASAPICapturer::WASAPICapturer() {
    LOG_INFO("[WASAPICapturer] Created");
}

WASAPICapturer::~WASAPICapturer() {
    Stop();
    LOG_INFO("[WASAPICapturer] Destroyed");
}

std::vector<AudioDeviceInfo> WASAPICapturer::EnumerateDevices(bool input) {
    std::vector<AudioDeviceInfo> devices;

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        LOG_ERROR("[WASAPICapturer] Failed to create device enumerator: 0x" + 
                  std::to_string(hr));
        return devices;
    }

    Microsoft::WRL::ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(input ? eCapture : eRender, 
                                         DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        LOG_ERROR("[WASAPICapturer] Failed to enumerate audio endpoints: 0x" + 
                  std::to_string(hr));
        return devices;
    }

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; i++) {
        Microsoft::WRL::ComPtr<IMMDevice> device;
        hr = collection->Item(i, &device);
        if (FAILED(hr)) continue;

        AudioDeviceInfo info;

        // 获取设备 ID
        LPWSTR device_id = nullptr;
        hr = device->GetId(&device_id);
        if (SUCCEEDED(hr)) {
            // 转换宽字符到 UTF-8
            int size = WideCharToMultiByte(CP_UTF8, 0, device_id, -1, nullptr, 0, nullptr, nullptr);
            if (size > 0) {
                info.id.resize(size - 1);
                WideCharToMultiByte(CP_UTF8, 0, device_id, -1, &info.id[0], size, nullptr, nullptr);
            }
            CoTaskMemFree(device_id);
        }

        // 获取设备名称
        info.name = GetDeviceName(device.Get());

        devices.push_back(info);
        LOG_INFO("[WASAPICapturer] Found device: " + info.name + " (id: " + info.id + ")");
    }

    return devices;
}

bool WASAPICapturer::Initialize(WASAPISourceType type, const std::string& device_id, 
                                 bool use_default_device) {
    source_type_ = type;
    device_id_ = device_id;
    use_default_device_ = use_default_device;

    // 创建设备枚举器
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&enumerator_));
    if (FAILED(hr)) {
        LOG_ERROR("[WASAPICapturer] Failed to create device enumerator: 0x" + 
                  std::to_string(hr));
        return false;
    }

    // 初始化设备
    device_ = InitDevice(device_id, use_default_device);
    if (!device_) {
        return false;
    }

    // 初始化音频客户端
    audio_client_ = InitClient(device_.Get());
    if (!audio_client_) {
        return false;
    }

    device_name_ = GetDeviceName(device_.Get());
    LOG_INFO("[WASAPICapturer] Initialized: " + device_name_ +
             ", sample_rate=" + std::to_string(sample_rate_) +
             ", channels=" + std::to_string(channels_) +
             ", format=" + (is_float_format_ ? "FLOAT" : "PCM") +
             ", bits=" + std::to_string(bits_per_sample_));

    // 🔧 调试：检查设备音量
    {
        Microsoft::WRL::ComPtr<IAudioEndpointVolume> volume;
        HRESULT hr = device_->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, 
                                       reinterpret_cast<void**>(volume.GetAddressOf()));
        if (SUCCEEDED(hr) && volume) {
            BOOL mute = FALSE;
            volume->GetMute(&mute);
            float vol = 0.0f;
            volume->GetMasterVolumeLevelScalar(&vol);
            LOG_INFO("[WASAPICapturer] Device volume: " + std::to_string(vol * 100) + "%, mute=" + std::to_string(mute));
        } else {
            LOG_INFO("[WASAPICapturer] Could not get volume control");
        }
    }

    return true;
}

void WASAPICapturer::SetCallback(AudioDataCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_ = std::move(callback);
}

bool WASAPICapturer::Start() {
    if (is_capturing_) {
        return true;
    }

    if (!audio_client_) {
        LOG_ERROR("[WASAPICapturer] Audio client not initialized");
        return false;
    }

    // 创建事件
    stop_event_ = CreateEvent(nullptr, true, false, nullptr);
    receive_event_ = CreateEvent(nullptr, false, false, nullptr);

    if (!stop_event_ || !receive_event_) {
        LOG_ERROR("[WASAPICapturer] Failed to create events");
        return false;
    }

    // 初始化捕获客户端
    capture_client_ = InitCapture(audio_client_.Get(), receive_event_);
    if (!capture_client_) {
        CloseHandle(stop_event_);
        CloseHandle(receive_event_);
        return false;
    }

    // 清空累积缓冲区
    {
        std::lock_guard<std::mutex> lock(accumulator_mutex_);
        audio_accumulator_.clear();
        total_samples_ = 0;
    }

    // 启动采集
    HRESULT hr = audio_client_->Start();
    if (FAILED(hr)) {
        LOG_ERROR("[WASAPICapturer] Failed to start audio client: 0x" + std::to_string(hr));
        capture_client_.Reset();
        CloseHandle(stop_event_);
        CloseHandle(receive_event_);
        return false;
    }

    // 创建采集线程
    is_capturing_ = true;
    capture_thread_ = CreateThread(nullptr, 0, CaptureThread, this, 0, nullptr);

    if (!capture_thread_) {
        LOG_ERROR("[WASAPICapturer] Failed to create capture thread");
        is_capturing_ = false;
        audio_client_->Stop();
        capture_client_.Reset();
        CloseHandle(stop_event_);
        CloseHandle(receive_event_);
        return false;
    }

    LOG_INFO("[WASAPICapturer] Started capturing");
    return true;
}

void WASAPICapturer::Stop() {
    if (!is_capturing_) {
        return;
    }

    is_capturing_ = false;

    // 发送停止信号
    if (stop_event_) {
        SetEvent(stop_event_);
    }

    // 等待线程结束
    if (capture_thread_) {
        WaitForSingleObject(capture_thread_, 5000);
        CloseHandle(capture_thread_);
        capture_thread_ = nullptr;
    }

    // 停止音频客户端
    if (audio_client_) {
        audio_client_->Stop();
    }

    // 清理资源
    capture_client_.Reset();

    if (stop_event_) {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
    if (receive_event_) {
        CloseHandle(receive_event_);
        receive_event_ = nullptr;
    }

    LOG_INFO("[WASAPICapturer] Stopped capturing");
}

Microsoft::WRL::ComPtr<IMMDevice> WASAPICapturer::InitDevice(const std::string& device_id, 
                                                               bool use_default_device) {
    Microsoft::WRL::ComPtr<IMMDevice> device;
    HRESULT hr;

    if (use_default_device) {
        // 获取默认音频端点
        EDataFlow data_flow = (source_type_ == WASAPISourceType::Input) ? eCapture : eRender;
        ERole role = eConsole;

        hr = enumerator_->GetDefaultAudioEndpoint(data_flow, role, &device);
        if (FAILED(hr)) {
            LOG_ERROR("[WASAPICapturer] Failed to get default audio endpoint: 0x" + 
                      std::to_string(hr));
            return nullptr;
        }

        // 🔧 调试：打印默认设备名称
        {
            std::string device_name = GetDeviceName(device.Get());
            LOG_INFO("[WASAPICapturer] Default device: " + device_name);
        }
    } else {
        // 根据设备 ID 获取设备
        int size = MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(), -1, nullptr, 0);
        std::wstring w_device_id(size - 1, 0);
        MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(), -1, &w_device_id[0], size);

        hr = enumerator_->GetDevice(w_device_id.c_str(), &device);
        if (FAILED(hr)) {
            LOG_ERROR("[WASAPICapturer] Failed to get device by ID: 0x" + std::to_string(hr));
            return nullptr;
        }
    }

    // 🔧 调试：检查设备状态
    {
        DWORD state = DEVICE_STATE_NOTPRESENT;
        hr = device->GetState(&state);
        if (SUCCEEDED(hr)) {
            std::string state_str;
            switch (state) {
                case DEVICE_STATE_ACTIVE: state_str = "ACTIVE"; break;
                case DEVICE_STATE_DISABLED: state_str = "DISABLED"; break;
                case DEVICE_STATE_NOTPRESENT: state_str = "NOTPRESENT"; break;
                case DEVICE_STATE_UNPLUGGED: state_str = "UNPLUGGED"; break;
                default: state_str = "UNKNOWN"; break;
            }
            LOG_INFO("[WASAPICapturer] Device state: " + state_str + " (code: " + std::to_string(state) + ")");
        } else {
            LOG_WARNING("[WASAPICapturer] Failed to get device state: 0x" + std::to_string(hr));
        }
    }

    return device;
}

Microsoft::WRL::ComPtr<IAudioClient> WASAPICapturer::InitClient(IMMDevice* device) {
    Microsoft::WRL::ComPtr<IAudioClient> client;

    // 激活音频客户端
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, 
                                   &client);
    if (FAILED(hr)) {
        LOG_ERROR("[WASAPICapturer] Failed to activate audio client: 0x" + std::to_string(hr));
        return nullptr;
    }

    // 获取混音格式
    WAVEFORMATEX* wfex = nullptr;
    hr = client->GetMixFormat(&wfex);
    if (FAILED(hr)) {
        LOG_ERROR("[WASAPICapturer] Failed to get mix format: 0x" + std::to_string(hr));
        return nullptr;
    }

    // 🔧 调试：详细打印格式信息
    std::string format_type;
    switch (wfex->wFormatTag) {
        case WAVE_FORMAT_PCM: format_type = "PCM"; break;
        case WAVE_FORMAT_IEEE_FLOAT: format_type = "IEEE_FLOAT"; break;
        case WAVE_FORMAT_EXTENSIBLE: format_type = "EXTENSIBLE"; break;
        default: format_type = "UNKNOWN(" + std::to_string(wfex->wFormatTag) + ")"; break;
    }
    LOG_INFO("[WASAPICapturer] Format tag: " + format_type + 
             ", " + std::to_string(wfex->wBitsPerSample) + " bits, " +
             "nBlockAlign=" + std::to_string(wfex->nBlockAlign) +
             ", nAvgBytesPerSec=" + std::to_string(wfex->nAvgBytesPerSec));

    // 如果是 EXTENSIBLE 格式，尝试获取子格式信息
    if (wfex->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfex->cbSize >= 22) {
        WAVEFORMATEXTENSIBLE* wfext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wfex);
        // 检查是否是 PCM (00000001-0000-0010-8000-00AA00389B71) 或 FLOAT (00000003-0000-0010-8000-00AA00389B71)
        if (wfext->SubFormat.Data1 == 1) {
            LOG_INFO("[WASAPICapturer] SubFormat appears to be PCM");
        } else if (wfext->SubFormat.Data1 == 3) {
            LOG_INFO("[WASAPICapturer] SubFormat appears to be IEEE_FLOAT");
        } else {
            LOG_INFO("[WASAPICapturer] SubFormat Data1: " + std::to_string(wfext->SubFormat.Data1));
        }
        LOG_INFO("[WASAPICapturer] Samples: " + std::to_string(wfext->Samples.wValidBitsPerSample) +
                 " valid bits, " + std::to_string(wfext->Samples.wSamplesPerBlock) + " per block");
        LOG_INFO("[WASAPICapturer] ChannelMask: 0x" + std::to_string(wfext->dwChannelMask));
    }

    // 解析格式
    sample_rate_ = wfex->nSamplesPerSec;
    channels_ = wfex->nChannels;
    bits_per_sample_ = wfex->wBitsPerSample;

    // 检测格式类型
    if (wfex->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        is_float_format_ = true;
    } else if (wfex->wFormatTag == WAVE_FORMAT_PCM) {
        is_float_format_ = false;
    } else if (wfex->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfex->cbSize >= 22) {
        WAVEFORMATEXTENSIBLE* wfext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wfex);
        if (wfext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            is_float_format_ = true;
        } else if (wfext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
            is_float_format_ = false;
        }
    }

    LOG_INFO("[WASAPICapturer] Mix format: " + std::to_string(sample_rate_) + "Hz, " +
             std::to_string(channels_) + " channels, " + 
             std::to_string(bits_per_sample_) + " bits, " +
             (is_float_format_ ? "IEEE_FLOAT" : "PCM"));

    // 设置流标志
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (source_type_ == WASAPISourceType::DeviceOutput) {
        flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;  // 环回采集（桌面音频）
    }

    LOG_INFO("[WASAPICapturer] Using flags: 0x" + std::to_string(flags));

    // 初始化音频客户端
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 
                            BUFFER_TIME_100NS, 0, wfex, nullptr);
    CoTaskMemFree(wfex);

    if (FAILED(hr)) {
        LOG_ERROR("[WASAPICapturer] Failed to initialize audio client: 0x" + std::to_string(hr));
        return nullptr;
    }

    return client;
}

Microsoft::WRL::ComPtr<IAudioCaptureClient> WASAPICapturer::InitCapture(IAudioClient* client, 
                                                                          HANDLE event_handle) {
    Microsoft::WRL::ComPtr<IAudioCaptureClient> capture;

    // 获取捕获服务
    HRESULT hr = client->GetService(IID_PPV_ARGS(&capture));
    if (FAILED(hr)) {
        LOG_ERROR("[WASAPICapturer] Failed to get capture service: 0x" + std::to_string(hr));
        return nullptr;
    }

    // 设置事件句柄
    hr = client->SetEventHandle(event_handle);
    if (FAILED(hr)) {
        LOG_ERROR("[WASAPICapturer] Failed to set event handle: 0x" + std::to_string(hr));
        return nullptr;
    }

    return capture;
}

bool WASAPICapturer::ProcessCaptureData() {
    HRESULT hr;
    UINT32 capture_size = 0;
    int total_frames_captured = 0;

    while (true) {
        hr = capture_client_->GetNextPacketSize(&capture_size);
        if (FAILED(hr)) {
            if (hr != AUDCLNT_E_DEVICE_INVALIDATED) {
                LOG_ERROR("[WASAPICapturer] GetNextPacketSize failed: 0x" + std::to_string(hr));
            }
            return false;
        }

        if (capture_size == 0) {
            break;
        }

        BYTE* buffer = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 pos = 0, ts = 0;

        hr = capture_client_->GetBuffer(&buffer, &frames, &flags, &pos, &ts);
        if (FAILED(hr)) {
            if (hr != AUDCLNT_E_DEVICE_INVALIDATED) {
                LOG_ERROR("[WASAPICapturer] GetBuffer failed: 0x" + std::to_string(hr));
            }
            return false;
        }

        // 🔧 诊断：打印flags状态
        static int buffer_flags_counter = 0;
        buffer_flags_counter++;
        if (buffer_flags_counter <= 5) {
            // AUDCLNT_BUFFERFLAGS_SILENT = 0x00000001
            bool is_silent_flag = (flags & 0x1) != 0;
            char flags_hex[20];
            snprintf(flags_hex, sizeof(flags_hex), "0x%x", flags);
            LOG_INFO("[WASAPICapturer] Buffer #" + std::to_string(buffer_flags_counter) +
                     ": frames=" + std::to_string(frames) +
                     ", flags=" + std::string(flags_hex) +
                     ", SILENT_FLAG=" + std::to_string(is_silent_flag));
        }

        // 处理静音标志
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            // 使用静音缓冲区（始终使用float格式）
            uint32_t required_floats = frames * channels_;
            uint32_t required_bytes = required_floats * sizeof(float);
            if (silence_buffer_.size() < required_bytes) {
                silence_buffer_.resize(required_bytes, 0);
            }
            buffer = silence_buffer_.data();

            // 累积静音数据（已经是float格式）
            {
                std::lock_guard<std::mutex> lock(accumulator_mutex_);
                const float* float_data = reinterpret_cast<const float*>(buffer);
                size_t total_floats = frames * channels_;
                for (size_t i = 0; i < total_floats; i++) {
                    audio_accumulator_.push_back(float_data[i]);
                }
                total_frames_captured += frames;
            }
            // 🔧 调试：静音数据也计入 RMS 计算
            if (buffer_flags_counter <= 3) {
                const float* float_data = reinterpret_cast<const float*>(buffer);
                double rms = 0.0;
                int check_samples = (std::min)((int)(frames * channels_), 100);
                double sum_squares = 0;
                for (int i = 0; i < check_samples; i++) {
                    sum_squares += float_data[i] * float_data[i];
                }
                rms = std::sqrt(sum_squares / check_samples);
                LOG_INFO("[WASAPICapturer] Silence buffer #" + std::to_string(buffer_flags_counter) +
                         ": frames=" + std::to_string(frames) +
                         ", RMS=" + std::to_string(rms) + " (silent flag set)");
            }
        } else {
            // 🔧 诊断：检查实际音频数据
            static int real_data_counter = 0;
            real_data_counter++;

            // 根据格式类型转换数据
            std::lock_guard<std::mutex> lock(accumulator_mutex_);
            size_t total_samples = frames * channels_;

            if (is_float_format_) {
                // IEEE_FLOAT 格式 - 直接拷贝
                const float* float_data = reinterpret_cast<const float*>(buffer);
                if (real_data_counter <= 5 && frames > 0) {
                    float first_val = float_data[0];
                    float second_val = float_data[1];
                    float fifth_val = channels_ > 1 ? float_data[4] : float_data[1];
                    double rms = 0.0;
                    int check_samples = (std::min)((int)total_samples, 100);
                    double sum_squares = 0;
                    for (int i = 0; i < check_samples; i++) {
                        sum_squares += float_data[i] * float_data[i];
                    }
                    rms = std::sqrt(sum_squares / check_samples);
                    
                    // 🔧 改进：显示更多诊断信息
                    float max_val = 0.0f;
                    for (size_t i = 0; i <(std::min)(total_samples, (size_t)100); i++) {
                        max_val = (std::max)(max_val, std::abs(float_data[i]));
                    }
                    
                    LOG_INFO("[WASAPICapturer] Float audio #" + std::to_string(real_data_counter) +
                             ": frames=" + std::to_string(frames) +
                             ", first=[" + std::to_string(first_val) + "," + std::to_string(second_val) + "]" +
                             ", RMS=" + std::to_string(rms) +
                             ", max=" + std::to_string(max_val) +
                             ", format=" + (is_float_format_ ? "FLOAT" : "PCM"));
                }
                for (size_t i = 0; i < total_samples; i++) {
                    audio_accumulator_.push_back(float_data[i]);
                }
            } else {
                // PCM 格式 - 需要转换为float
                if (bits_per_sample_ == 16) {
                    const int16_t* pcm_data = reinterpret_cast<const int16_t*>(buffer);
                    if (real_data_counter <= 5 && frames > 0) {
                        int16_t first_val = pcm_data[0];
                        int16_t second_val = pcm_data[1];
                        double rms = 0.0;
                        int check_samples = (std::min)((int)total_samples, 100);
                        double sum_squares = 0;
                        for (int i = 0; i < check_samples; i++) {
                            double val = pcm_data[i] / 32768.0;
                            sum_squares += val * val;
                        }
                        rms = std::sqrt(sum_squares / check_samples);
                        
                        // 🔧 改进：显示更多诊断信息
                        float max_val = 0.0f;
                        for (size_t i = 0; i < (std::min)(total_samples, (size_t)100); i++) {
                            max_val = (std::max)(max_val, std::abs(pcm_data[i] / 32768.0f));
                        }
                        
                        LOG_INFO("[WASAPICapturer] PCM16 audio #" + std::to_string(real_data_counter) +
                                 ": frames=" + std::to_string(frames) +
                                 ", first=[" + std::to_string(first_val) + "," + std::to_string(second_val) + "]" +
                                 ", RMS=" + std::to_string(rms) +
                                 ", max=" + std::to_string(max_val));
                    }
                    // 转换为 float (-1.0 到 1.0)
                    for (size_t i = 0; i < total_samples; i++) {
                        audio_accumulator_.push_back(pcm_data[i] / 32768.0f);
                    }
                } else if (bits_per_sample_ == 24) {
                    // 24位PCM处理
                    const uint8_t* pcm24_data = buffer;
                    for (size_t i = 0; i < total_samples; i++) {
                        int32_t sample = (pcm24_data[i * 3] | (pcm24_data[i * 3 + 1] << 8) |
                                         ((int8_t)pcm24_data[i * 3 + 2] << 16));
                        audio_accumulator_.push_back(sample / 8388608.0f);
                    }
                } else if (bits_per_sample_ == 32) {
                    const int32_t* pcm_data = reinterpret_cast<const int32_t*>(buffer);
                    for (size_t i = 0; i < total_samples; i++) {
                        audio_accumulator_.push_back(pcm_data[i] / 2147483648.0f);
                    }
                } else {
                    // 不支持的位深度，使用静音
                    for (size_t i = 0; i < total_samples; i++) {
                        audio_accumulator_.push_back(0.0f);
                    }
                    LOG_WARNING("[WASAPICapturer] Unsupported bits per sample: " + std::to_string(bits_per_sample_));
                }
            }
            total_frames_captured += frames;
        }

        // 释放缓冲区
        capture_client_->ReleaseBuffer(frames);
    }

    // 调试日志：每次采集的帧数
    if (total_frames_captured > 0) {
        static int capture_log_counter = 0;
        capture_log_counter++;
        if (capture_log_counter <= 10 || capture_log_counter % 50 == 0) {
            LOG_INFO("[WASAPICapturer] Captured " + std::to_string(total_frames_captured) + 
                     " frames, accumulator now has " + 
                     std::to_string(audio_accumulator_.size() / channels_) + " samples");
        }
    }

    return true;
}

void WASAPICapturer::OutputFixedFrames() {
    // 计算固定帧大小（样本数）
    size_t frame_size_samples = AUDIO_FRAMES_PER_CALLBACK * channels_;

    std::lock_guard<std::mutex> lock(accumulator_mutex_);

    // 每次输出固定 1024 帧
    int frames_output = 0;
    while (audio_accumulator_.size() >= frame_size_samples) {
        // 创建固定大小的帧
        std::vector<float> frame_data(frame_size_samples);
        for (size_t i = 0; i < frame_size_samples; i++) {
            frame_data[i] = audio_accumulator_.front();
            audio_accumulator_.pop_front();
        }

        // 调用回调
        {
            std::lock_guard<std::mutex> cb_lock(callback_mutex_);
            if (callback_) {
                // 🔧 调试：保存原始音频数据到 PCM 文件
                // 保存为 16-bit PCM 格式（而非 float），以便播放器能正常播放
                //static FILE* pcm_file = nullptr;
                //static int pcm_frame_count = 0;
                //if (!pcm_file && pcm_frame_count == 0) {
                //    pcm_file = fopen("D:\\test_mic.pcm", "wb");
                //    if (pcm_file) {
                //        LOG_INFO("[WASAPICapturer] Opened PCM file for writing (16-bit PCM)");
                //    }
                //}
                //if (pcm_file && pcm_frame_count < 100) {
                //    // 将 float 数据转换为 16-bit PCM
                //    std::vector<int16_t> pcm16_data(frame_size_samples);
                //    for (size_t i = 0; i < frame_size_samples; i++) {
                //        float sample = frame_data[i];
                //        // 限制范围到 [-1.0, 1.0]
                //        sample = (std::max)(-1.0f, (std::min)(1.0f, sample));
                //        pcm16_data[i] = static_cast<int16_t>(sample * 32767.0f);
                //    }
                //    fwrite(pcm16_data.data(), sizeof(int16_t), pcm16_data.size(), pcm_file);
                //    pcm_frame_count++;
                //    if (pcm_frame_count == 100) {
                //        fclose(pcm_file);
                //        pcm_file = nullptr;
                //        LOG_INFO("[WASAPICapturer] Saved 100 frames (16-bit PCM) to PCM file");
                //    }
                //}

                // 计算 PTS（基于累计样本数）
                // PTS = total_samples / sample_rate * 1000000000 (纳秒)
                int64_t timestamp = (total_samples_ * 1000000000ULL) / sample_rate_;
                
                callback_(frame_data.data(), AUDIO_FRAMES_PER_CALLBACK, 
                         sample_rate_, channels_, timestamp);
            }
        }

        // 增加累计样本数
        total_samples_ += AUDIO_FRAMES_PER_CALLBACK;
        frames_output++;
    }

    // 调试日志：每次输出的帧数
    if (frames_output > 0) {
        static int output_log_counter = 0;
        output_log_counter++;
        if (output_log_counter <= 10 || output_log_counter % 50 == 0) {
            LOG_INFO("[WASAPICapturer] Output " + std::to_string(frames_output) + 
                     " x 1024 frames, total_output=" + std::to_string(total_samples_) +
                     ", remaining=" + std::to_string(audio_accumulator_.size() / channels_) + " samples");
        }
    }
}

DWORD WINAPI WASAPICapturer::CaptureThread(LPVOID param) {
    WASAPICapturer* capturer = static_cast<WASAPICapturer*>(param);

    // 初始化 COM
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool com_initialized = SUCCEEDED(hr);

    // 设置线程为音频线程（提高优先级）
    DWORD task_index = 0;
    HANDLE mm_handle = AvSetMmThreadCharacteristics(L"Audio", &task_index);

    // 等待的事件
    HANDLE events[] = {
        capturer->stop_event_,
        capturer->receive_event_
    };

    // 诊断计数器
    static int log_counter = 0;

    while (capturer->is_capturing_) {
        // 等待事件
        DWORD wait_result = WaitForMultipleObjects(2, events, false, 100);

        if (wait_result == WAIT_OBJECT_0) {
            // 停止事件
            break;
        }

        if (wait_result == WAIT_OBJECT_0 + 1 || wait_result == WAIT_TIMEOUT) {
            // 接收事件或超时，处理数据
            if (!capturer->ProcessCaptureData()) {
                // 设备可能已断开
                break;
            }

            // 从累积缓冲区输出固定 1024 帧
            capturer->OutputFixedFrames();

            // 诊断：每 100 次记录一次状态
            log_counter++;
            if (log_counter % 100 == 0) {
                std::lock_guard<std::mutex> acc_lock(capturer->accumulator_mutex_);
                LOG_INFO("[WASAPICapturer] Accumulator: " + 
                         std::to_string(capturer->audio_accumulator_.size() / capturer->channels_) +
                         " samples, total_output=" + std::to_string(capturer->total_samples_));
            }
        }
    }

    // 清理
    if (mm_handle) {
        AvRevertMmThreadCharacteristics(mm_handle);
    }
    if (com_initialized) {
        CoUninitialize();
    }

    return 0;
}

std::string WASAPICapturer::GetDeviceName(IMMDevice* device) {
    if (!device) return "";

    Microsoft::WRL::ComPtr<IPropertyStore> props;
    HRESULT hr = device->OpenPropertyStore(STGM_READ, &props);
    if (FAILED(hr)) {
        return "";
    }

    PROPVARIANT name_prop;
    PropVariantInit(&name_prop);

    hr = props->GetValue(PKEY_Device_FriendlyName, &name_prop);
    if (FAILED(hr)) {
        PropVariantClear(&name_prop);
        return "";
    }

    // 转换宽字符到 UTF-8
    std::string name;
    if (name_prop.pwszVal) {
        int size = WideCharToMultiByte(CP_UTF8, 0, name_prop.pwszVal, -1, nullptr, 0, nullptr, nullptr);
        if (size > 0) {
            name.resize(size - 1);
            WideCharToMultiByte(CP_UTF8, 0, name_prop.pwszVal, -1, &name[0], size, nullptr, nullptr);
        }
    }

    PropVariantClear(&name_prop);
    return name;
}

} // namespace live_assistant
