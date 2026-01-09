#include "scene_manager/source_factory.h"
#include "common/log.h"
#include "common/error.h"
#include "video_engine/video_engine.h"

namespace live_assistant {

std::shared_ptr<Source> SourceFactory::create_source(Source::Type type, const std::string& id, const std::string& name) {
    LOG_INFO("Creating source of type " + std::to_string(static_cast<int>(type)) + " with id: " + id);
    
    switch (type) {
        case Source::Type::VIDEO_CAPTURE:
            return create_camera_source(id, name);
        case Source::Type::AUDIO_CAPTURE:
            return create_audio_source(id, name);
        case Source::Type::SCREEN_CAPTURE:
            return create_screen_source(id, name);
        case Source::Type::FILE_SOURCE:
            return nullptr;
        case Source::Type::NETWORK_SOURCE:
            return nullptr;
        default:
            LOG_ERROR("Unknown source type: " + std::to_string(static_cast<int>(type)));
            return nullptr;
    }
}

std::shared_ptr<Source> SourceFactory::create_camera_source(const std::string& id, const std::string& name) {
    LOG_INFO("Creating camera source: " + id);
    return std::make_shared<CameraSource>(id, name);
}

std::shared_ptr<Source> SourceFactory::create_screen_source(const std::string& id, const std::string& name) {
    LOG_INFO("Creating screen source: " + id);
    return std::make_shared<ScreenSource>(id, name);
}

std::shared_ptr<Source> SourceFactory::create_image_source(const std::string& id, const std::string& image_path, const std::string& name) {
    LOG_INFO("Creating image source: " + id + " with path: " + image_path);
    return std::make_shared<ImageSource>(id, image_path, name);
}

std::shared_ptr<Source> SourceFactory::create_media_file_source(const std::string& id, const std::string& file_path, const std::string& name) {
    LOG_INFO("Creating media file source: " + id + " with path: " + file_path);
    return std::make_shared<MediaFileSource>(id, file_path, name);
}

std::shared_ptr<Source> SourceFactory::create_audio_source(const std::string& id, const std::string& name) {
    LOG_INFO("Creating audio source: " + id);
    return std::make_shared<AudioSourceImpl>(id, name);
}

VideoSource::VideoSource(const std::string& id, Source::Type type) : Source(id, type) {
    LOG_INFO("VideoSource constructor");
}

AudioSource::AudioSource(const std::string& id, Source::Type type) : Source(id, type) {
    LOG_INFO("AudioSource constructor");
}

CameraSource::CameraSource(const std::string& id, const std::string& name) 
    : VideoSource(id, Source::Type::VIDEO_CAPTURE), name_(name) {
    LOG_INFO("CameraSource constructor: " + id);
}

CameraSource::~CameraSource() {
    shutdown();
    LOG_INFO("CameraSource destructor: " + get_id());
}

bool CameraSource::initialize() {
    if (initialized_) {
        return true;
    }
    
    LOG_INFO("Initializing camera source: " + get_id());
    initialized_ = true;
    return true;
}

bool CameraSource::shutdown() {
    if (!initialized_) {
        return true;
    }
    
    LOG_INFO("Shutting down camera source: " + get_id());
    stop();
    initialized_ = false;
    return true;
}

bool CameraSource::start() {
    if (!initialized_) {
        return false;
    }
    
    if (running_) {
        return true;
    }
    
    LOG_INFO("Starting camera source: " + get_id());
    running_ = true;
    return true;
}

bool CameraSource::stop() {
    if (!running_) {
        return true;
    }
    
    LOG_INFO("Stopping camera source: " + get_id());
    running_ = false;
    return true;
}

bool CameraSource::is_running() const {
    return running_;
}

std::shared_ptr<VideoFrame> CameraSource::get_video_frame() {
    return nullptr;
}

bool CameraSource::get_frame(std::vector<uint8_t>& frame_data, int& width, int& height) {
    if (!running_) {
        return false;
    }
    
    width = 0;
    height = 0;
    frame_data.clear();
    return true;
}

bool CameraSource::select_camera(const std::string& camera_id) {
    selected_camera_id_ = camera_id;
    LOG_INFO("Selected camera: " + camera_id + " for source: " + get_id());
    return true;
}

std::vector<std::string> CameraSource::get_available_cameras() const {
    return {"Camera 1", "Camera 2"};
}

ScreenSource::ScreenSource(const std::string& id, const std::string& name) 
    : VideoSource(id, Source::Type::VIDEO_CAPTURE), name_(name) {
    LOG_INFO("ScreenSource constructor: " + id);
}

ScreenSource::~ScreenSource() {
    shutdown();
    LOG_INFO("ScreenSource destructor: " + get_id());
}

bool ScreenSource::initialize() {
    if (initialized_) {
        return true;
    }
    
    LOG_INFO("Initializing screen source: " + get_id());
    initialized_ = true;
    return true;
}

bool ScreenSource::shutdown() {
    if (!initialized_) {
        return true;
    }
    
    LOG_INFO("Shutting down screen source: " + get_id());
    stop();
    initialized_ = false;
    return true;
}

bool ScreenSource::start() {
    if (!initialized_) {
        return false;
    }
    
    if (running_) {
        return true;
    }
    
    LOG_INFO("Starting screen source: " + get_id());
    running_ = true;
    return true;
}

bool ScreenSource::stop() {
    if (!running_) {
        return true;
    }
    
    LOG_INFO("Stopping screen source: " + get_id());
    running_ = false;
    return true;
}

bool ScreenSource::is_running() const {
    return running_;
}

bool ScreenSource::get_frame(std::vector<uint8_t>& frame_data, int& width, int& height) {
    if (!running_) {
        return false;
    }
    
    width = 0;
    height = 0;
    frame_data.clear();
    return true;
}

void ScreenSource::push_frame(const QImage& image) {
    std::lock_guard<std::mutex> lk(latest_frame_mutex_);
    latest_frame_ = image.copy();
}

QImage ScreenSource::get_latest_frame() const {
    std::lock_guard<std::mutex> lk(latest_frame_mutex_);
    return latest_frame_.copy();
}

bool ScreenSource::select_screen(int screen_index) {
    selected_screen_index_ = screen_index;
    LOG_INFO("Selected screen index: " + std::to_string(screen_index) + " for source: " + get_id());
    return true;
}

bool ScreenSource::set_capture_region(int x, int y, int width, int height) {
    capture_x_ = x;
    capture_y_ = y;
    capture_width_ = width;
    capture_height_ = height;
    
    LOG_INFO("Set capture region: " + std::to_string(x) + "," + 
              std::to_string(y) + "," + std::to_string(width) + "," + 
              std::to_string(height) + " for source: " + get_id());
    return true;
}

std::vector<std::string> ScreenSource::get_available_screens() const {
    return {"Screen 1", "Screen 2"};
}

ImageSource::ImageSource(const std::string& id, const std::string& image_path, const std::string& name) 
    : VideoSource(id, Source::Type::FILE_SOURCE), name_(name), image_path_(image_path) {
    LOG_INFO("ImageSource constructor: " + id + " with path: " + image_path);
}

ImageSource::~ImageSource() {
    shutdown();
    LOG_INFO("ImageSource destructor: " + get_id());
}

bool ImageSource::initialize() {
    if (initialized_) {
        return true;
    }
    
    LOG_INFO("Initializing image source: " + get_id());
    initialized_ = true;
    return true;
}

bool ImageSource::shutdown() {
    if (!initialized_) {
        return true;
    }
    
    LOG_INFO("Shutting down image source: " + get_id());
    stop();
    image_data_.clear();
    initialized_ = false;
    return true;
}

bool ImageSource::start() {
    if (!initialized_) {
        return false;
    }
    
    if (running_) {
        return true;
    }
    
    LOG_INFO("Starting image source: " + get_id());
    running_ = true;
    return true;
}

bool ImageSource::stop() {
    if (!running_) {
        return true;
    }
    
    LOG_INFO("Stopping image source: " + get_id());
    running_ = false;
    return true;
}

bool ImageSource::is_running() const {
    return running_;
}

bool ImageSource::get_frame(std::vector<uint8_t>& frame_data, int& width, int& height) {
    if (!running_) {
        return false;
    }
    
    width = image_width_;
    height = image_height_;
    frame_data = image_data_;
    return true;
}

MediaFileSource::MediaFileSource(const std::string& id, const std::string& file_path, const std::string& name) 
    : VideoSource(id, Source::Type::FILE_SOURCE), name_(name), file_path_(file_path) {
    LOG_INFO("MediaFileSource constructor: " + id + " with path: " + file_path);
}

MediaFileSource::~MediaFileSource() {
    shutdown();
    LOG_INFO("MediaFileSource destructor: " + get_id());
}

bool MediaFileSource::initialize() {
    if (initialized_) {
        return true;
    }
    
    LOG_INFO("Initializing media file source: " + get_id());
    initialized_ = true;
    return true;
}

bool MediaFileSource::shutdown() {
    if (!initialized_) {
        return true;
    }
    
    LOG_INFO("Shutting down media file source: " + get_id());
    stop();
    initialized_ = false;
    return true;
}

bool MediaFileSource::start() {
    if (!initialized_) {
        return false;
    }
    
    if (running_) {
        return true;
    }
    
    LOG_INFO("Starting media file source: " + get_id());
    running_ = true;
    return true;
}

bool MediaFileSource::stop() {
    if (!running_) {
        return true;
    }
    
    LOG_INFO("Stopping media file source: " + get_id());
    running_ = false;
    return true;
}

bool MediaFileSource::is_running() const {
    return running_;
}

bool MediaFileSource::get_frame(std::vector<uint8_t>& frame_data, int& width, int& height) {
    if (!running_) {
        return false;
    }
    
    width = width_;
    height = height_;
    frame_data.clear();
    return true;
}

AudioSourceImpl::AudioSourceImpl(const std::string& id, const std::string& name) 
    : AudioSource(id, Source::Type::AUDIO_CAPTURE), name_(name) {
    LOG_INFO("AudioSourceImpl constructor: " + id);
}

AudioSourceImpl::~AudioSourceImpl() {
    shutdown();
    LOG_INFO("AudioSourceImpl destructor: " + get_id());
}

bool AudioSourceImpl::initialize() {
    if (initialized_) {
        return true;
    }
    
    LOG_INFO("Initializing audio source: " + get_id());
    initialized_ = true;
    return true;
}

bool AudioSourceImpl::shutdown() {
    if (!initialized_) {
        return true;
    }
    
    LOG_INFO("Shutting down audio source: " + get_id());
    stop();
    initialized_ = false;
    return true;
}

bool AudioSourceImpl::start() {
    if (!initialized_) {
        return false;
    }
    
    if (running_) {
        return true;
    }
    
    LOG_INFO("Starting audio source: " + get_id());
    running_ = true;
    return true;
}

bool AudioSourceImpl::stop() {
    if (!running_) {
        return true;
    }
    
    LOG_INFO("Stopping audio source: " + get_id());
    running_ = false;
    return true;
}

bool AudioSourceImpl::is_running() const {
    return running_;
}

bool AudioSourceImpl::get_audio_data(std::vector<int16_t>& audio_data, int& sample_rate, int& channels) {
    if (!running_) {
        return false;
    }
    
    sample_rate = sample_rate_;
    channels = channels_;
    audio_data.clear();
    return true;
}

} // namespace live_assistant
