#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <mutex>

#include "encoder/encoder_config.h"
#include "stream_pusher/stream_config.h"

namespace live_assistant {

// 配置变更观察者接口
class IConfigObserver {
public:
    virtual ~IConfigObserver() = default;

    // 视频编码配置变更通知
    virtual void on_video_config_changed(const VideoEncoderConfig& config) = 0;

    // 音频编码配置变更通知
    virtual void on_audio_config_changed(const AudioEncoderConfig& config) = 0;

    // 推流配置变更通知
    virtual void on_stream_config_changed(const StreamConfig& config) = 0;

    // 画布配置变更通知
    virtual void on_canvas_config_changed(int width, int height, bool is_portrait) = 0;
};

// 统一的直播会话配置管理器（单例 + 观察者模式）
class StreamSessionConfig : public std::enable_shared_from_this<StreamSessionConfig> {
public:
    static std::shared_ptr<StreamSessionConfig> instance();

    // 禁止拷贝
    StreamSessionConfig(const StreamSessionConfig&) = delete;
    StreamSessionConfig& operator=(const StreamSessionConfig&) = delete;

    // ===== 视频编码配置 =====
    const VideoEncoderConfig& get_video_config() const { return video_config_; }
    void set_video_config(const VideoEncoderConfig& config);

    // ===== 音频编码配置 =====
    const AudioEncoderConfig& get_audio_config() const { return audio_config_; }
    void set_audio_config(const AudioEncoderConfig& config);

    // ===== 推流配置 =====
    const StreamConfig& get_stream_config() const { return stream_config_; }
    void set_stream_config(const StreamConfig& config);

    // ===== 画布配置 =====
    int get_canvas_width() const { return canvas_width_; }
    int get_canvas_height() const { return canvas_height_; }
    bool is_portrait_mode() const { return is_portrait_mode_; }
    void set_canvas_config(int width, int height, bool is_portrait);

    // ===== 观察者管理 =====
    void add_observer(IConfigObserver* observer);
    void remove_observer(IConfigObserver* observer);

    // ===== 便捷方法：根据画布尺寸自动设置视频编码参数 =====
    void init_video_config_from_canvas();

    // ===== 便捷方法：设置推流地址 =====
    void set_rtmp_url(const std::string& server_url, const std::string& stream_key);

    // ===== 获取默认配置 =====
    static VideoEncoderConfig get_default_video_config(int width, int height, bool is_portrait);
    static AudioEncoderConfig get_default_audio_config(int sample_rate, int channels);
    static StreamConfig get_default_stream_config(const std::string& url);

private:
    StreamSessionConfig();

    void notify_video_config_changed();
    void notify_audio_config_changed();
    void notify_stream_config_changed();
    void notify_canvas_config_changed();

    // 配置
    VideoEncoderConfig video_config_;
    AudioEncoderConfig audio_config_;
    StreamConfig stream_config_;

    // 画布配置
    int canvas_width_ = 1280;
    int canvas_height_ = 720;
    bool is_portrait_mode_ = false;

    // 观察者列表
    std::vector<IConfigObserver*> observers_;
    mutable std::mutex observer_mutex_;
};

// 便捷的观察者基类实现（提供默认空实现）
class SimpleConfigObserver : public IConfigObserver {
public:
    void on_video_config_changed(const VideoEncoderConfig& config) override {}
    void on_audio_config_changed(const AudioEncoderConfig& config) override {}
    void on_stream_config_changed(const StreamConfig& config) override {}
    void on_canvas_config_changed(int width, int height, bool is_portrait) override {}
};

} // namespace live_assistant
