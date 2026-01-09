#include "audio_engine/audio_engine.h"
#include "common/log.h"
#include "common/error.h"

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#endif

namespace live_assistant {

AudioEngine::AudioEngine() {
    LOG_INFO("Initialized AudioEngine");
}

AudioEngine::~AudioEngine() {
    shutdown();
    LOG_INFO("AudioEngine destructor called");
}

bool AudioEngine::initialize(int sample_rate, int channels) {
    sample_rate_ = sample_rate;
    channels_ = channels;

    LOG_INFO("AudioEngine initialized with sample rate: " +
             std::to_string(sample_rate) + "Hz, " +
             std::to_string(channels) + " channels");

#ifdef _WIN32
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        if (hr != RPC_E_CHANGED_MODE) {
            LOG_ERROR("Failed to initialize COM: " + std::to_string(hr));
            return false;
        }
        LOG_INFO("COM already initialized, continuing with existing initialization");
    }

    ErrorCode result = initialize_wasapi();
    if (result != ErrorCode::SUCCESS) {
        CoUninitialize();
        return false;
    }
#endif

    return true;
}

bool AudioEngine::shutdown() {
    if (is_capturing_) {
        stop_capture();
    }

#ifdef _WIN32
    cleanup_wasapi();
    CoUninitialize();
#endif

    audio_sources_.clear();
    LOG_INFO("AudioEngine shutdown");
    return true;
}

bool AudioEngine::start_capture() {
    if (is_capturing_) {
        LOG_WARNING("Audio capture already started");
        return true;
    }
    
    stop_capture_flag_ = false;
    
#ifdef _WIN32
    capture_thread_ = std::thread(&AudioEngine::capture_thread_func, this);
#endif
    
    is_capturing_ = true;
    LOG_INFO("Started audio capture");
    return true;
}

bool AudioEngine::stop_capture() {
    if (!is_capturing_) {
        LOG_WARNING("Audio capture already stopped");
        return true;
    }
    
    stop_capture_flag_ = true;
    
#ifdef _WIN32
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
#endif
    
    is_capturing_ = false;
    LOG_INFO("Stopped audio capture");
    return true;
}

std::vector<std::string> AudioEngine::get_available_microphones() {
    std::vector<std::string> microphones;
    
#ifdef _WIN32
    HRESULT hr = S_OK;
    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDeviceCollection* pCollection = nullptr;
    
    // Create a device enumerator
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&pEnumerator));
    
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create device enumerator: " + std::to_string(hr));
        return microphones;
    }
    
    // Enumerate audio input devices
    hr = pEnumerator->EnumAudioEndpoints(
        eCapture, DEVICE_STATE_ACTIVE,
        &pCollection);
    
    if (FAILED(hr)) {
        LOG_ERROR("Failed to enumerate audio endpoints: " + std::to_string(hr));
        pEnumerator->Release();
        return microphones;
    }
    
    UINT count = 0;
    hr = pCollection->GetCount(&count);
    
    if (FAILED(hr)) {
        LOG_ERROR("Failed to get device count: " + std::to_string(hr));
        pCollection->Release();
        pEnumerator->Release();
        return microphones;
    }
    
    for (UINT i = 0; i < count; i++) {
        IMMDevice* pDevice = nullptr;
        hr = pCollection->Item(i, &pDevice);
        
        if (FAILED(hr)) {
            LOG_ERROR("Failed to get device at index " + std::to_string(i) + ": " + std::to_string(hr));
            continue;
        }
        
        IPropertyStore* pProps = nullptr;
        hr = pDevice->OpenPropertyStore(
            STGM_READ, &pProps);
        
        if (FAILED(hr)) {
            LOG_ERROR("Failed to open property store for device: " + std::to_string(hr));
            pDevice->Release();
            continue;
        }
        
        PROPVARIANT varName;
        PropVariantInit(&varName);
        
        // Get the device friendly name
        hr = pProps->GetValue(
            PKEY_Device_FriendlyName, &varName);
        
        if (SUCCEEDED(hr) && varName.vt == VT_LPWSTR) {
            std::wstring wname(varName.pwszVal);
            std::string name(wname.begin(), wname.end());
            microphones.push_back(name);
            LOG_INFO("Found microphone: " + name);
        }
        
        PropVariantClear(&varName);
        pProps->Release();
        pDevice->Release();
    }
    
    pCollection->Release();
    pEnumerator->Release();
    
    LOG_INFO("Enumerated " + std::to_string(microphones.size()) + " microphones");
#endif
    
    // Fallback if no microphones found or on non-Windows platforms
    if (microphones.empty()) {
        microphones.push_back("Default Microphone");
        LOG_WARNING("No microphones found, using fallback");
    }
    
    return microphones;
}

bool AudioEngine::select_microphone(const std::string& mic_id) {
    // 检查麦克风ID是否为空
    if (mic_id.empty()) {
        LOG_ERROR("Invalid microphone ID: empty string");
        return false;
    }
    
    // 获取可用麦克风列表，检查指定的麦克风是否存在
    std::vector<std::string> available_mics = get_available_microphones();
    bool mic_exists = false;
    
    for (const auto& mic : available_mics) {
        if (mic == mic_id) {
            mic_exists = true;
            break;
        }
    }
    
    if (!mic_exists) {
        LOG_ERROR("Microphone not found: " + mic_id);
        LOG_ERROR("Available microphones:");
        for (const auto& mic : available_mics) {
            LOG_ERROR("  - " + mic);
        }
        return false;
    }
    
    selected_microphone_ = mic_id;
    LOG_INFO("Selected microphone: " + mic_id);
    return true;
}

bool AudioEngine::enable_noise_suppression(bool enable) {
    noise_suppression_enabled_ = enable;
    LOG_INFO(std::string("Noise suppression ") + (enable ? "enabled" : "disabled"));
    return true;
}

bool AudioEngine::enable_echo_cancellation(bool enable) {
    echo_cancellation_enabled_ = enable;
    LOG_INFO(std::string("Echo cancellation ") + (enable ? "enabled" : "disabled"));
    return true;
}

std::shared_ptr<AudioFrame> AudioEngine::get_audio_frame() {
    std::shared_ptr<AudioFrame> frame = nullptr;
    
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (!frame_queue_.empty()) {
            frame = frame_queue_.front();
            frame_queue_.pop();
        }
    }
    
    if (!frame) {
        frame = std::make_shared<AudioFrame>(sample_rate_, channels_, 0);
    }
    
    return frame;
}

bool AudioEngine::add_audio_source(std::shared_ptr<AudioEngine> source) {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    audio_sources_.push_back(source);
    LOG_INFO("Added audio source");
    return true;
}

bool AudioEngine::remove_audio_source(std::shared_ptr<AudioEngine> source) {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    
    auto it = std::remove_if(audio_sources_.begin(), audio_sources_.end(),
        [&source](const std::weak_ptr<AudioEngine>& weak) {
            auto shared = weak.lock();
            return shared == source || shared == nullptr;
        });
    
    if (it != audio_sources_.end()) {
        audio_sources_.erase(it, audio_sources_.end());
        LOG_INFO("Removed audio source");
        return true;
    }
    return false;
}

int AudioEngine::get_sample_rate() const {
    return sample_rate_;
}

int AudioEngine::get_channels() const {
    return channels_;
}

#ifdef _WIN32
ErrorCode AudioEngine::initialize_wasapi() {
    HRESULT hr = S_OK;
    IMMDevice* pDevice = nullptr;
    IAudioClient* pAudioClient = nullptr;
    IAudioCaptureClient* pCaptureClient = nullptr;
    WAVEFORMATEXTENSIBLE* pFormatExtensible = nullptr;
    REFERENCE_TIME hnsRequestedDuration = 10000000;
    REFERENCE_TIME hnsActualDuration = 0;

    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator_));
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create device enumerator: " + std::to_string(hr));
        return ErrorCode::AUDIO_DEVICE_ERROR;
    }

    hr = enumerator_->GetDefaultAudioEndpoint(
        eCapture, eConsole, &audio_device_);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to get default audio device: " + std::to_string(hr));
        enumerator_->Release();
        enumerator_ = nullptr;
        return ErrorCode::AUDIO_DEVICE_ERROR;
    }

    hr = audio_device_->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(&audio_client_));
    if (FAILED(hr)) {
        LOG_ERROR("Failed to activate audio client: " + std::to_string(hr));
        audio_device_->Release();
        enumerator_->Release();
        audio_device_ = nullptr;
        enumerator_ = nullptr;
        return ErrorCode::AUDIO_DEVICE_ERROR;
    }

    WAVEFORMATEX* pWaveFormat = nullptr;
    hr = audio_client_->GetMixFormat(&pWaveFormat);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to get mix format: " + std::to_string(hr));
        audio_client_->Release();
        audio_device_->Release();
        enumerator_->Release();
        audio_client_ = nullptr;
        audio_device_ = nullptr;
        enumerator_ = nullptr;
        return ErrorCode::AUDIO_FORMAT_ERROR;
    }
    format_ = pWaveFormat;

    pWaveFormat->wFormatTag = WAVE_FORMAT_PCM;
    pWaveFormat->nChannels = channels_;
    pWaveFormat->nSamplesPerSec = sample_rate_;
    pWaveFormat->wBitsPerSample = 16;
    pWaveFormat->nBlockAlign = (pWaveFormat->nChannels * pWaveFormat->wBitsPerSample) / 8;
    pWaveFormat->nAvgBytesPerSec = pWaveFormat->nSamplesPerSec * pWaveFormat->nBlockAlign;
    pWaveFormat->cbSize = 0;

    hr = audio_client_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
        hnsRequestedDuration,
        0,
        static_cast<WAVEFORMATEX*>(format_),
        nullptr);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to initialize audio client: " + std::to_string(hr));
        CoTaskMemFree(format_);
        audio_client_->Release();
        audio_device_->Release();
        enumerator_->Release();
        format_ = nullptr;
        audio_client_ = nullptr;
        audio_device_ = nullptr;
        enumerator_ = nullptr;
        return ErrorCode::AUDIO_INIT_ERROR;
    }

    hr = audio_client_->GetService(
        __uuidof(IAudioCaptureClient),
        reinterpret_cast<void**>(&capture_client_));
    if (FAILED(hr)) {
        LOG_ERROR("Failed to get capture client: " + std::to_string(hr));
        CoTaskMemFree(static_cast<WAVEFORMATEX*>(format_));
        audio_client_->Release();
        audio_device_->Release();
        enumerator_->Release();
        format_ = nullptr;
        audio_client_ = nullptr;
        audio_device_ = nullptr;
        enumerator_ = nullptr;
        return ErrorCode::AUDIO_DEVICE_ERROR;
    }

    LOG_INFO("WASAPI initialized successfully");
    return ErrorCode::SUCCESS;
}

void AudioEngine::cleanup_wasapi() {
    if (capture_client_) {
        capture_client_->Release();
        capture_client_ = nullptr;
    }

    if (audio_client_) {
        audio_client_->Stop();
        audio_client_->Release();
        audio_client_ = nullptr;
    }

    if (format_) {
        CoTaskMemFree(static_cast<WAVEFORMATEX*>(format_));
        format_ = nullptr;
    }

    if (audio_device_) {
        audio_device_->Release();
        audio_device_ = nullptr;
    }

    if (enumerator_) {
        enumerator_->Release();
        enumerator_ = nullptr;
    }

    LOG_INFO("WASAPI cleaned up");
}

void AudioEngine::capture_thread_func() {
    HRESULT hr = S_OK;
    UINT32 numFramesAvailable = 0;
    DWORD flags = 0;
    BYTE* pData = nullptr;
    UINT32 numFramesToRead = 0;

    hr = audio_client_->Start();
    if (FAILED(hr)) {
        LOG_ERROR("Failed to start audio client: " + std::to_string(hr));
        return;
    }

    while (!stop_capture_flag_) {
        hr = capture_client_->GetNextPacketSize(&numFramesAvailable);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to get next packet size: " + std::to_string(hr));
            break;
        }

        while (numFramesAvailable > 0) {
            hr = capture_client_->GetBuffer(
                &pData, &numFramesToRead, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                LOG_ERROR("Failed to get buffer: " + std::to_string(hr));
                break;
            }

            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                LOG_WARNING("Audio buffer discontinuity detected");
                capture_client_->ReleaseBuffer(numFramesToRead);
                hr = capture_client_->GetNextPacketSize(&numFramesAvailable);
                continue;
            }

            WAVEFORMATEX* pWaveFormat = static_cast<WAVEFORMATEX*>(format_);
            
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                memset(pData, 0, numFramesToRead * pWaveFormat->nBlockAlign);
            }

            auto frame = std::make_shared<AudioFrame>(sample_rate_, channels_, numFramesToRead);
            
            memcpy(frame->raw_data, pData, numFramesToRead * pWaveFormat->nBlockAlign);
            
            frame->convert_raw_to_float();

            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                frame_queue_.push(frame);
            }
            frame_cv_.notify_one();

            hr = capture_client_->ReleaseBuffer(numFramesToRead);
            if (FAILED(hr)) {
                LOG_ERROR("Failed to release buffer: " + std::to_string(hr));
                break;
            }

            hr = capture_client_->GetNextPacketSize(&numFramesAvailable);
            if (FAILED(hr)) {
                LOG_ERROR("Failed to get next packet size: " + std::to_string(hr));
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    hr = audio_client_->Stop();
    if (FAILED(hr)) {
        LOG_ERROR("Failed to stop audio client: " + std::to_string(hr));
    }

    LOG_INFO("Audio capture thread exited");
}
#endif

AudioFrame::AudioFrame(int sample_rate, int channels, int samples) 
    : sample_rate(sample_rate), channels(channels), samples(samples) {
    data = new float[samples * channels];
    memset(data, 0, samples * channels * sizeof(float));
    
    raw_data = new int16_t[samples * channels];
    memset(raw_data, 0, samples * channels * sizeof(int16_t));
}

AudioFrame::~AudioFrame() {
    if (data) {
        delete[] data;
        data = nullptr;
    }
    if (raw_data) {
        delete[] raw_data;
        raw_data = nullptr;
    }
}

AudioFrame::AudioFrame(AudioFrame&& other) noexcept 
    : data(other.data), raw_data(other.raw_data), sample_rate(other.sample_rate), 
      channels(other.channels), samples(other.samples) {
    other.data = nullptr;
    other.raw_data = nullptr;
    other.samples = 0;
}

AudioFrame& AudioFrame::operator=(AudioFrame&& other) noexcept {
    if (this != &other) {
        if (data) {
            delete[] data;
        }
        if (raw_data) {
            delete[] raw_data;
        }
        data = other.data;
        raw_data = other.raw_data;
        sample_rate = other.sample_rate;
        channels = other.channels;
        samples = other.samples;
        
        other.data = nullptr;
        other.raw_data = nullptr;
        other.samples = 0;
    }
    return *this;
}

void AudioFrame::convert_raw_to_float() {
    if (!raw_data || !data) {
        return;
    }
    
    for (int i = 0; i < samples * channels; ++i) {
        data[i] = static_cast<float>(raw_data[i]) / 32768.0f;
    }
}

void AudioFrame::convert_float_to_raw() {
    if (!data || !raw_data) {
        return;
    }

    for (int i = 0; i < samples * channels; ++i) {
        float scaled = data[i] * 32768.0f;
        if (scaled > 32767.0f) scaled = 32767.0f;
        if (scaled < -32768.0f) scaled = -32768.0f;
        raw_data[i] = static_cast<int16_t>(scaled);
    }
}

} // namespace live_assistant
