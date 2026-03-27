#include "common/stream_session_config.h"
#include "common/log.h"
#include <QString>

namespace live_assistant {

std::shared_ptr<StreamSessionConfig> StreamSessionConfig::instance() {
    static std::shared_ptr<StreamSessionConfig> instance(new StreamSessionConfig());
    return instance;
}

StreamSessionConfig::StreamSessionConfig() {
    // 初始化默认视频编码配置
    video_config_ = get_default_video_config(canvas_width_, canvas_height_, is_portrait_mode_);

    // 初始化默认音频编码配置
    audio_config_ = get_default_audio_config(48000, 2);

    // 初始化默认推流配置
    stream_config_ = get_default_stream_config("rtmp://47.92.156.37:1935/live/aaa");

    LOG_INFO("[StreamSessionConfig] Initialized with default configs");
}

void StreamSessionConfig::set_video_config(const VideoEncoderConfig& config) {
    video_config_ = config;
    notify_video_config_changed();
    LOG_INFO(QString("[StreamSessionConfig] Video config updated: %1x%2 @ %3kbps, fps=%4, gop=%5")
                 .arg(config.width).arg(config.height)
                 .arg(config.bitrate / 1000)
                 .arg(config.fps)
                 .arg(config.gop).toStdString());
}

void StreamSessionConfig::set_audio_config(const AudioEncoderConfig& config) {
    audio_config_ = config;
    notify_audio_config_changed();
    LOG_INFO(QString("[StreamSessionConfig] Audio config updated: %1Hz %2ch %3kbps")
                 .arg(config.sample_rate).arg(config.channels).arg(config.bitrate / 1000).toStdString());
}

void StreamSessionConfig::set_stream_config(const StreamConfig& config) {
    stream_config_ = config;
    notify_stream_config_changed();
    LOG_INFO("[StreamSessionConfig] Stream config updated: " + stream_config_.server_url);
}

void StreamSessionConfig::set_canvas_config(int width, int height, bool is_portrait) {
    canvas_width_ = width;
    canvas_height_ = height;
    is_portrait_mode_ = is_portrait;
    notify_canvas_config_changed();
    LOG_INFO(QString("[StreamSessionConfig] Canvas config updated: %1x%2 portrait=%3")
                 .arg(width).arg(height).arg(is_portrait).toStdString());
}

void StreamSessionConfig::add_observer(IConfigObserver* observer) {
    std::lock_guard<std::mutex> lock(observer_mutex_);
    // 检查是否已存在
    for (auto* obs : observers_) {
        if (obs == observer) {
            LOG_WARNING("[StreamSessionConfig] Observer already registered");
            return;
        }
    }
    observers_.push_back(observer);
    LOG_INFO("[StreamSessionConfig] Observer added, total: " + std::to_string(observers_.size()));
}

void StreamSessionConfig::remove_observer(IConfigObserver* observer) {
    std::lock_guard<std::mutex> lock(observer_mutex_);
    observers_.erase(
        std::remove(observers_.begin(), observers_.end(), observer),
        observers_.end()
    );
    LOG_INFO("[StreamSessionConfig] Observer removed, total: " + std::to_string(observers_.size()));
}

void StreamSessionConfig::init_video_config_from_canvas() {
    video_config_ = get_default_video_config(canvas_width_, canvas_height_, is_portrait_mode_);
    notify_video_config_changed();
    LOG_INFO(QString("[StreamSessionConfig] Video config auto-initialized from canvas: %1x%2 portrait=%3")
                 .arg(canvas_width_).arg(canvas_height_).arg(is_portrait_mode_).toStdString());
}

void StreamSessionConfig::set_rtmp_url(const std::string& server_url, const std::string& stream_key) {
    stream_config_.server_url = server_url;
    stream_config_.stream_key = stream_key;
    notify_stream_config_changed();
    LOG_INFO("[StreamSessionConfig] RTMP URL updated: " + server_url + "/" + stream_key);
}

VideoEncoderConfig StreamSessionConfig::get_default_video_config(int width, int height, bool is_portrait) {
    VideoEncoderConfig config;
    config.width = width;
    config.height = height;
    config.fps = 30;
    config.gop = 60;  // 2秒一个关键帧
    config.b_frames_enabled = false;

    // 根据横竖屏模式设置码率
    config.bitrate = is_portrait ? 2000000 : 2500000;

    return config;
}

AudioEncoderConfig StreamSessionConfig::get_default_audio_config(int sample_rate, int channels) {
    AudioEncoderConfig config;
    config.sample_rate = sample_rate;
    config.channels = channels;
    config.bitrate = 128000;

    return config;
}

StreamConfig StreamSessionConfig::get_default_stream_config(const std::string& url) {
    StreamConfig config;
    config.server_url = url;
    config.stream_key = "";
    config.protocol = StreamProtocol::RTMP;
    config.send_buffer_ms = 500;
    config.max_queue_size = 100;
    config.auto_reconnect = true;
    config.max_reconnect_attempts = 5;
    config.reconnect_interval_sec = 3;
    config.low_latency = true;
    config.use_interleaved_write = true;

    return config;
}

void StreamSessionConfig::notify_video_config_changed() {
    std::lock_guard<std::mutex> lock(observer_mutex_);
    for (auto* obs : observers_) {
        if (obs) {
            obs->on_video_config_changed(video_config_);
        }
    }
}

void StreamSessionConfig::notify_audio_config_changed() {
    std::lock_guard<std::mutex> lock(observer_mutex_);
    for (auto* obs : observers_) {
        if (obs) {
            obs->on_audio_config_changed(audio_config_);
        }
    }
}

void StreamSessionConfig::notify_stream_config_changed() {
    std::lock_guard<std::mutex> lock(observer_mutex_);
    for (auto* obs : observers_) {
        if (obs) {
            obs->on_stream_config_changed(stream_config_);
        }
    }
}

void StreamSessionConfig::notify_canvas_config_changed() {
    std::lock_guard<std::mutex> lock(observer_mutex_);
    for (auto* obs : observers_) {
        if (obs) {
            obs->on_canvas_config_changed(canvas_width_, canvas_height_, is_portrait_mode_);
        }
    }
}

} // namespace live_assistant
