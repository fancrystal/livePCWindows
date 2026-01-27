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
    // Ensure WASAPI initialized before starting capture thread
    if (!audio_client_ || !capture_client_) {
        ErrorCode init_res = initialize_wasapi();
        if (init_res != ErrorCode::SUCCESS) {
            LOG_ERROR("start_capture: WASAPI not initialized, aborting start_capture");
            return false;
        }
    }
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

    // Apply volume control and mute to the audio frame
    if (frame && frame->raw_data) {
        if (microphone_muted_) {
            // Fill with silence
            memset(frame->raw_data, 0, frame->samples * frame->channels * sizeof(int16_t));
        } else if (microphone_volume_ < 1.0f) {
            // Apply volume scaling
            float scale = microphone_volume_;
            int16_t* data = frame->raw_data;
            for (int i = 0; i < frame->samples * frame->channels; ++i) {
                data[i] = static_cast<int16_t>(data[i] * scale);
            }
        }
    }

    // Diagnostic: log when returning a frame (rate-limited)
    static int get_frame_log_skips = 0;
    if (frame) {
        bool all_zero = true;
        if (frame->raw_data) {
            int total = frame->samples * frame->channels;
            for (int i = 0; i < total; ++i) {
                if (frame->raw_data[i] != 0) { all_zero = false; break; }
            }
        }
        if (get_frame_log_skips == 0) {
            LOG_INFO("AudioEngine::get_audio_frame returning frame: samples=" + std::to_string(frame->samples) +
                     ", channels=" + std::to_string(frame->channels) +
                     ", muted=" + (microphone_muted_ ? "yes" : "no") +
                     ", volume=" + std::to_string(microphone_volume_) +
                     ", all_zero=" + (all_zero ? "yes" : "no"));
        }
        get_frame_log_skips = (get_frame_log_skips + 1) % 20;
    } else {
        if (get_frame_log_skips == 0) {
            LOG_DEBUG("AudioEngine::get_audio_frame - no frame available in queue");
        }
        get_frame_log_skips = (get_frame_log_skips + 1) % 20;
    }

    return frame;
}

// Volume control implementation
bool AudioEngine::set_microphone_volume(float volume) {
    microphone_volume_ = (std::max)(0.0f, (std::min)(volume, 1.0f));
    LOG_INFO("Microphone volume set to: " + std::to_string(microphone_volume_));
    return true;
}

float AudioEngine::get_microphone_volume() const {
    return microphone_volume_;
}

bool AudioEngine::set_microphone_mute(bool mute) {
    microphone_muted_ = mute;
    LOG_INFO(std::string("Microphone ") + (mute ? "muted" : "unmuted"));
    return true;
}

bool AudioEngine::get_microphone_mute() const {
    return microphone_muted_;
}

bool AudioEngine::set_speaker_volume(float volume) {
    speaker_volume_ = (std::max)(0.0f, (std::min)(volume, 1.0f));
    LOG_INFO("Speaker volume set to: " + std::to_string(speaker_volume_));

    // On Windows, we could adjust system speaker volume here
    // For now, just store the value
#ifdef _WIN32
    // TODO: Implement Windows system speaker volume control
#endif

    return true;
}

float AudioEngine::get_speaker_volume() const {
    return speaker_volume_;
}

bool AudioEngine::set_speaker_mute(bool mute) {
    speaker_muted_ = mute;
    LOG_INFO(std::string("Speaker ") + (mute ? "muted" : "unmuted"));

    // On Windows, we could mute/unmute system speaker here
    // For now, just store the value
#ifdef _WIN32
    // TODO: Implement Windows system speaker mute control
#endif

    return true;
}

bool AudioEngine::get_speaker_mute() const {
    return speaker_muted_;
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
        char buf[64];
        sprintf_s(buf, "0x%08x", static_cast<unsigned int>(hr));
        LOG_ERROR(std::string("Failed to activate audio client: hr=") + std::to_string(hr) + " (" + buf + ")");
        return ErrorCode::AUDIO_DEVICE_ERROR;
    }

    WAVEFORMATEX* pWaveFormat = nullptr;
    hr = audio_client_->GetMixFormat(&pWaveFormat);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to get mix format: " + std::to_string(hr));
        return ErrorCode::AUDIO_FORMAT_ERROR;
    }
    format_ = pWaveFormat;

    // Use device mix format as-is to avoid unsupported-format errors.
    // Update engine sample_rate_ / channels_ to match device if not explicitly configured.
    if (pWaveFormat) {
        if (sample_rate_ <= 0) sample_rate_ = pWaveFormat->nSamplesPerSec;
        if (channels_ <= 0) channels_ = pWaveFormat->nChannels;
        LOG_INFO("Device mix format: sample_rate=" + std::to_string(pWaveFormat->nSamplesPerSec) +
                 ", channels=" + std::to_string(pWaveFormat->nChannels) +
                 ", wFormatTag=" + std::to_string(pWaveFormat->wFormatTag));
    }

    hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, pWaveFormat, nullptr);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to initialize audio client: " + std::to_string(hr));
        return ErrorCode::AUDIO_INIT_ERROR;
    }

    hr = audio_client_->GetService(__uuidof(IAudioCaptureClient), (void**)&capture_client_);
    if (FAILED(hr)) {
        char buf2[64];
        sprintf_s(buf2, "0x%08x", static_cast<unsigned int>(hr));
        LOG_ERROR(std::string("Failed to get capture client: hr=") + std::to_string(hr) + " (" + buf2 + ")");
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
                // Convert device buffer to int16_t raw_data according to mix format
                WAVEFORMATEX* wf = static_cast<WAVEFORMATEX*>(format_);
                if (wf && wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                    // float32 -> int16
                    float* fdata = reinterpret_cast<float*>(pData);
                    int total = numFramesToRead * channels_;
                    for (int i = 0; i < total; ++i) {
                        float v = fdata[i];
                        // clamp
                        if (v > 1.0f) v = 1.0f;
                        if (v < -1.0f) v = -1.0f;
                        int32_t iv = static_cast<int32_t>(std::round(v * 32767.0f));
                        if (iv > 32767) iv = 32767;
                        if (iv < -32768) iv = -32768;
                        frame->raw_data[i] = static_cast<int16_t>(iv);
                    }
                } else if (wf && wf->wFormatTag == WAVE_FORMAT_PCM && wf->wBitsPerSample == 16) {
                    // int16 PCM
                    memcpy(frame->raw_data, pData, numFramesToRead * channels_ * sizeof(int16_t));
                } else if (wf && wf->wFormatTag == WAVE_FORMAT_PCM && wf->wBitsPerSample == 32) {
                    // int32 PCM -> int16
                    int32_t* idata = reinterpret_cast<int32_t*>(pData);
                    int total = numFramesToRead * channels_;
                    for (int i = 0; i < total; ++i) {
                        int64_t iv = idata[i] >> 16; // downscale 32->16
                        if (iv > 32767) iv = 32767;
                        if (iv < -32768) iv = -32768;
                        frame->raw_data[i] = static_cast<int16_t>(iv);
                    }
                } else {
                    // Unknown format: try to memcpy as int16 as fallback
                    memcpy(frame->raw_data, pData, numFramesToRead * channels_ * sizeof(int16_t));
                }
                // Post-conversion: check clipping and apply soft attenuation if needed
                int total = numFramesToRead * channels_;
                int32_t maxAbs = 0;
                for (int i = 0; i < total; ++i) {
                    int32_t v = std::abs(static_cast<int32_t>(frame->raw_data[i]));
                    if (v > maxAbs) maxAbs = v;
                }
                const int32_t clipThreshold = 30000;
                if (maxAbs > clipThreshold) {
                    double scale = static_cast<double>(clipThreshold) / static_cast<double>(maxAbs);
                    for (int i = 0; i < total; ++i) {
                        int32_t val = static_cast<int32_t>(std::round(frame->raw_data[i] * scale));
                        if (val > 32767) val = 32767;
                        if (val < -32768) val = -32768;
                        frame->raw_data[i] = static_cast<int16_t>(val);
                    }
                    LOG_INFO("Audio capture: applied attenuation scale=" + std::to_string(scale) + " due to peak=" + std::to_string(maxAbs));
                }
            }

            capture_client_->ReleaseBuffer(numFramesToRead);

            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                frame_queue_.push(frame);
            }
            frame_cv_.notify_one();
            
            // Diagnostic: log when we receive non-silent audio data (rate-limited)
            bool non_silent = false;
            if (frame->raw_data) {
                int64_t sum_abs = 0;
                int total = frame->samples * frame->channels;
                for (int i = 0; i < total; ++i) {
                    sum_abs += std::abs(frame->raw_data[i]);
                    if (sum_abs > 0) { non_silent = true; break; }
                }
            }
            static int non_silent_log_skips = 0;
            if (non_silent) {
                if (non_silent_log_skips == 0) {
                    LOG_INFO("Audio capture: received non-silent frame, samples=" + std::to_string(frame->samples) +
                             ", channels=" + std::to_string(frame->channels));
                }
                non_silent_log_skips = (non_silent_log_skips + 1) % 50; // log at most 1/50 frames
            }
            
            // Log every 10 captured frames a short summary to verify capture is active
            static int capture_summary_skips = 0;
            if (capture_summary_skips == 0) {
                bool muted = microphone_muted_;
                float vol = microphone_volume_;
                LOG_INFO("Audio capture: queued frame, samples=" + std::to_string(frame->samples) +
                         ", channels=" + std::to_string(frame->channels) +
                         ", non_silent=" + (non_silent ? "yes" : "no") +
                         ", muted=" + (muted ? "yes" : "no") +
                         ", volume=" + std::to_string(vol));
            }
            capture_summary_skips = (capture_summary_skips + 1) % 10;
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
