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
    if (is_capturing_) {
        stop_capture();
    }
    cleanup_wasapi();

    sample_rate_ = sample_rate;
    channels_ = channels;

    LOG_INFO("AudioEngine initialized with sample rate: " +
             std::to_string(sample_rate) + "Hz, " +
             std::to_string(channels) + " channels");

#ifdef _WIN32
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        LOG_ERROR("Failed to initialize COM: " + std::to_string(hr));
        return false;
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

#ifdef _WIN32
// Helper to convert wchar_t* to UTF-8 std::string
static std::string wide_to_utf8(const wchar_t* wstr) {
    if (!wstr) return {};
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    if (size_needed <= 0) return {};
    std::string strTo(size_needed - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &strTo[0], size_needed, NULL, NULL);
    return strTo;
}
#endif

std::vector<AudioEngine::AudioDeviceInfo> AudioEngine::get_available_microphones() {
    std::vector<AudioDeviceInfo> microphones;

#ifdef _WIN32
    HRESULT hr = S_OK;
    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDeviceCollection* pCollection = nullptr;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create device enumerator: " + std::to_string(hr));
        return microphones;
    }

    hr = pEnumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pCollection);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to enumerate audio endpoints: " + std::to_string(hr));
        pEnumerator->Release();
        return microphones;
    }

    UINT count = 0;
    pCollection->GetCount(&count);

    for (UINT i = 0; i < count; i++) {
        IMMDevice* pDevice = nullptr;
        if (FAILED(pCollection->Item(i, &pDevice))) continue;

        LPWSTR pwszID = nullptr;
        std::string id_str;
        if (SUCCEEDED(pDevice->GetId(&pwszID))) {
            id_str = wide_to_utf8(pwszID);
            CoTaskMemFree(pwszID);
        }

        IPropertyStore* pProps = nullptr;
        std::string name_str;
        if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps))) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName))) {
                name_str = wide_to_utf8(varName.pwszVal);
                PropVariantClear(&varName);
            }
            pProps->Release();
        }

        if (!id_str.empty() && !name_str.empty()) {
            microphones.push_back({id_str, name_str});
            LOG_INFO("Found microphone: " + name_str + " (ID: " + id_str + ")");
        }

        pDevice->Release();
    }

    pCollection->Release();
    pEnumerator->Release();
#endif

    if (microphones.empty()) {
        microphones.push_back({"default", "Default Microphone"});
        LOG_WARNING("No microphones found, using fallback");
    }

    return microphones;
}

bool AudioEngine::select_microphone(const std::string& mic_id) {
    if (mic_id.empty()) {
        LOG_ERROR("Invalid microphone ID: empty string");
        return false;
    }
    selected_microphone_id_ = mic_id;
    LOG_INFO("Selected microphone ID: " + mic_id);
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
            return !shared || shared == source;
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

std::string AudioEngine::get_selected_microphone_id() const {
    return selected_microphone_id_;
}

#ifdef _WIN32
ErrorCode AudioEngine::initialize_wasapi() {
    HRESULT hr;
    cleanup_wasapi();

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator_);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create device enumerator: " + std::to_string(hr));
        return ErrorCode::AUDIO_DEVICE_ERROR;
    }

    if (!selected_microphone_id_.empty() && selected_microphone_id_ != "default") {
        std::wstring w_id(selected_microphone_id_.begin(), selected_microphone_id_.end());
        hr = enumerator_->GetDevice(w_id.c_str(), &audio_device_);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to get selected audio device, falling back to default.");
            selected_microphone_id_.clear(); // Clear to fallback
        }
    }

    if (!audio_device_) {
        hr = enumerator_->GetDefaultAudioEndpoint(eCapture, eConsole, &audio_device_);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to get default audio device: " + std::to_string(hr));
            return ErrorCode::AUDIO_DEVICE_ERROR;
        }
    }

    hr = audio_device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audio_client_);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to activate audio client: " + std::to_string(hr));
        return ErrorCode::AUDIO_DEVICE_ERROR;
    }

    WAVEFORMATEX* pWaveFormat = nullptr;
    hr = audio_client_->GetMixFormat(&pWaveFormat);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to get mix format: " + std::to_string(hr));
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

    hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, pWaveFormat, nullptr);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to initialize audio client: " + std::to_string(hr));
        return ErrorCode::AUDIO_INIT_ERROR;
    }

    hr = audio_client_->GetService(__uuidof(IAudioCaptureClient), (void**)&capture_client_);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to get capture client: " + std::to_string(hr));
        return ErrorCode::AUDIO_DEVICE_ERROR;
    }

    LOG_INFO("WASAPI initialized successfully");
    return ErrorCode::SUCCESS;
}

void AudioEngine::cleanup_wasapi() {
    if (capture_client_) { capture_client_->Release(); capture_client_ = nullptr; }
    if (audio_client_) { audio_client_->Stop(); audio_client_->Release(); audio_client_ = nullptr; }
    if (format_) { CoTaskMemFree(format_); format_ = nullptr; }
    if (audio_device_) { audio_device_->Release(); audio_device_ = nullptr; }
    if (enumerator_) { enumerator_->Release(); enumerator_ = nullptr; }
    LOG_INFO("WASAPI cleaned up");
}

void AudioEngine::capture_thread_func() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HRESULT hr = audio_client_->Start();
    if (FAILED(hr)) {
        LOG_ERROR("Failed to start audio client: " + std::to_string(hr));
        CoUninitialize();
        return;
    }

    while (!stop_capture_flag_) {
        UINT32 numFramesAvailable;
        hr = capture_client_->GetNextPacketSize(&numFramesAvailable);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to get next packet size: " + std::to_string(hr));
            break;
        }

        if (numFramesAvailable > 0) {
            BYTE* pData;
            UINT32 numFramesToRead;
            DWORD flags;
            hr = capture_client_->GetBuffer(&pData, &numFramesToRead, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                LOG_ERROR("Failed to get buffer: " + std::to_string(hr));
                break;
            }

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                // Handle silence if needed, for now just push silent data
            }

            auto frame = std::make_shared<AudioFrame>(sample_rate_, channels_, numFramesToRead);
            if (frame->raw_data) {
                 memcpy(frame->raw_data, pData, numFramesToRead * channels_ * sizeof(int16_t));
            }

            capture_client_->ReleaseBuffer(numFramesToRead);

            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                frame_queue_.push(frame);
            }
            frame_cv_.notify_one();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    audio_client_->Stop();
    CoUninitialize();
    LOG_INFO("Audio capture thread exited");
}
#endif

AudioFrame::AudioFrame(int sample_rate, int channels, int samples)
    : sample_rate(sample_rate), channels(channels), samples(samples) {
    size_t raw_size = samples * channels * sizeof(int16_t);
    raw_data = new (std::nothrow) int16_t[samples * channels];
    if(raw_data) memset(raw_data, 0, raw_size);

    size_t float_size = samples * channels * sizeof(float);
    data = new (std::nothrow) float[samples * channels];
    if(data) memset(data, 0, float_size);
}

AudioFrame::~AudioFrame() {
    delete[] data;
    delete[] raw_data;
}

AudioFrame::AudioFrame(AudioFrame&& other) noexcept
    : data(other.data), raw_data(other.raw_data), sample_rate(other.sample_rate),
      channels(other.channels), samples(other.samples) {
    other.data = nullptr;
    other.raw_data = nullptr;
}

AudioFrame& AudioFrame::operator=(AudioFrame&& other) noexcept {
    if (this != &other) {
        delete[] data;
        delete[] raw_data;
        data = other.data;
        raw_data = other.raw_data;
        sample_rate = other.sample_rate;
        channels = other.channels;
        samples = other.samples;
        other.data = nullptr;
        other.raw_data = nullptr;
    }
    return *this;
}

void AudioFrame::convert_raw_to_float() {
    if (!raw_data || !data) return;
    for (int i = 0; i < samples * channels; ++i) {
        data[i] = static_cast<float>(raw_data[i]) / 32768.0f;
    }
}

void AudioFrame::convert_float_to_raw() {
    if (!data || !raw_data) return;
    for (int i = 0; i < samples * channels; ++i) {
        float scaled = data[i] * 32768.0f;
        if (scaled > 32767.0f) scaled = 32767.0f;
        if (scaled < -32768.0f) scaled = -32768.0f;
        raw_data[i] = static_cast<int16_t>(scaled);
    }
}

} // namespace live_assistant
