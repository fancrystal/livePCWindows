#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <QImage>

// Forward declarations
struct VideoFrame;

namespace live_assistant {
class VideoEngine;
class WGCaptureSource;
}

#include "scene_manager/scene_manager.h"
#include "common/error.h"

namespace live_assistant {

// 源工厂类
class SourceFactory {
public:
    SourceFactory() = delete;
    ~SourceFactory() = delete;
    
    // 根据类型创建源
    static std::shared_ptr<Source> create_source(Source::Type type, const std::string& id, const std::string& name = "");
    
    // 创建摄像头源
    static std::shared_ptr<Source> create_camera_source(const std::string& id, const std::string& name = "");
    
    // 创建屏幕源
    static std::shared_ptr<Source> create_screen_source(const std::string& id, const std::string& name = "");
    
    // 创建图像源
    static std::shared_ptr<Source> create_image_source(const std::string& id, const std::string& image_path, const std::string& name = "");
    
    // 创建媒体文件源
    static std::shared_ptr<Source> create_media_file_source(const std::string& id, const std::string& file_path, const std::string& name = "");
    
    // 创建音频源
    static std::shared_ptr<Source> create_audio_source(const std::string& id, const std::string& name = "");
    
    // 创建WGC屏幕共享源
    static std::shared_ptr<Source> create_screen_share_source(const std::string& id, std::shared_ptr<WGCaptureSource> wgc_source);
};

// 所有视频源的基类
class VideoSource : public Source {
public:
    VideoSource(const std::string& id, Type type);
    virtual ~VideoSource() = default;
    
    // Source接口实现
    std::shared_ptr<AudioFrame> get_audio_frame() override { return nullptr; }
    std::shared_ptr<VideoFrame> get_video_frame() override { return nullptr; }
    std::string get_metadata() const override { return "type:video,id:" + get_id() + ",name:视频源"; }
    
    // 视频源特定方法
    virtual bool get_frame(std::vector<uint8_t>& frame_data, int& width, int& height) = 0;
};

// 所有音频源的基类
class AudioSource : public Source {
public:
    AudioSource(const std::string& id, Type type);
    virtual ~AudioSource() = default;
    
    // Source接口实现
    std::shared_ptr<AudioFrame> get_audio_frame() override { return nullptr; }
    std::shared_ptr<VideoFrame> get_video_frame() override { return nullptr; }
    std::string get_metadata() const override { return "type:audio,id:" + get_id(); }
    
    // 音频源特定方法
    virtual bool get_audio_data(std::vector<int16_t>& audio_data, int& sample_rate, int& channels) = 0;
};

// 摄像头源实现
class CameraSource : public VideoSource {
public:
    CameraSource(const std::string& id, const std::string& name = "");
    ~CameraSource() override;
    
    // Source接口实现
    bool initialize() override;
    bool shutdown() override;
    bool start() override;
    bool stop() override;
    bool is_running() const override;
    std::shared_ptr<VideoFrame> get_video_frame() override;
    std::string get_metadata() const override {
        std::string n = name_.empty() ? "摄像头" : name_;
        return "type:video,id:" + get_id() + ",name:" + n;
    }
    
    // VideoSource接口实现
    bool get_frame(std::vector<uint8_t>& frame_data, int& width, int& height) override;

    // 摄像头特定方法
    bool select_camera(const std::string& camera_id);
    std::vector<std::string> get_available_cameras() const;

    // 用于接收外部捕获的数据（如从 ICaptureSource）
    void push_frame(const QImage& image);
    QImage get_latest_frame() const;
    
private:
    std::string name_;
    std::string selected_camera_id_;
    bool initialized_ = false;
    bool running_ = false;

    // 用于存储最新帧（从外部捕获源接收）
    mutable std::mutex latest_frame_mutex_;
    QImage latest_frame_;
};

// 屏幕源实现
class ScreenSource : public VideoSource {
public:
    ScreenSource(const std::string& id, const std::string& name = "");
    ~ScreenSource() override;
    
    // Source接口实现
    bool initialize() override;
    bool shutdown() override;
    bool start() override;
    bool stop() override;
    bool is_running() const override;
    
    // VideoSource接口实现
    bool get_frame(std::vector<uint8_t>& frame_data, int& width, int& height) override;
    std::string get_metadata() const override {
        std::string n = name_.empty() ? "屏幕" : name_;
        return "type:video,id:" + get_id() + ",name:" + n;
    }
    
    // 屏幕特定方法
    bool select_screen(int screen_index);
    bool set_capture_region(int x, int y, int width, int height);
    std::vector<std::string> get_available_screens() const;
    
    // Push a captured frame (QImage) from capture pipeline
    void push_frame(const QImage& image);
    
    // Get the latest pushed frame (thread-safe). Returns null QImage if none.
    QImage get_latest_frame() const;
    
private:
    std::string name_;
    int selected_screen_index_ = 0;
    bool initialized_ = false;
    bool running_ = false;
    // 捕获区域
    int capture_x_ = 0;
    int capture_y_ = 0;
    int capture_width_ = 0;
    int capture_height_ = 0;
    // latest captured image (thread-safe)
    QImage latest_frame_;
    mutable std::mutex latest_frame_mutex_;
};

// WGC屏幕共享源实现
class WGCScreenShareSource : public VideoSource {
public:
    WGCScreenShareSource(const std::string& id, std::shared_ptr<WGCaptureSource> wgc_source);
    ~WGCScreenShareSource() override;
    
    // Source接口实现
    bool initialize() override;
    bool shutdown() override;
    bool start() override;
    bool stop() override;
    bool is_running() const override;
    std::shared_ptr<VideoFrame> get_video_frame() override;
    std::string get_metadata() const override {
        return "type:video,id:" + get_id() + ",name:屏幕共享";
    }
    
    // VideoSource接口实现
    bool get_frame(std::vector<uint8_t>& frame_data, int& width, int& height) override;
    
private:
    std::shared_ptr<WGCaptureSource> wgc_source_;
    bool initialized_ = false;
    bool running_ = false;
};

// 图像源实现
class ImageSource : public VideoSource {
public:
    ImageSource(const std::string& id, const std::string& image_path, const std::string& name = "");
    ~ImageSource() override;

    // Source接口实现
    bool initialize() override;
    bool shutdown() override;
    bool start() override;
    bool stop() override;
    bool is_running() const override;

    // VideoSource接口实现
    bool get_frame(std::vector<uint8_t>& frame_data, int& width, int& height) override;

private:
    std::string name_;
    std::string image_path_;
    std::vector<uint8_t> image_data_;
    int image_width_ = 0;
    int image_height_ = 0;
    bool initialized_ = false;
    bool running_ = false;
};

// MediaFileSource 已移至 media_pipeline/media_file_source.h，使用更完整的实现

// 音频源实现
class AudioSourceImpl : public AudioSource {
public:
    AudioSourceImpl(const std::string& id, const std::string& name = "");
    ~AudioSourceImpl() override;
    
    // Source接口实现
    bool initialize() override;
    bool shutdown() override;
    bool start() override;
    bool stop() override;
    bool is_running() const override;
    
    // AudioSource接口实现
    bool get_audio_data(std::vector<int16_t>& audio_data, int& sample_rate, int& channels) override;
    
private:
    std::string name_;
    bool initialized_ = false;
    bool running_ = false;
    int sample_rate_ = 44100;
    int channels_ = 2;
};

} // namespace live_assistant
