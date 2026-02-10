#ifndef MEDIA_FILE_SOURCE_H
#define MEDIA_FILE_SOURCE_H

#include "media_pipeline/source.h"
#include "http/live_item.h"
#include <QImage>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace live_assistant {

// 前向声明
struct AudioFrame;
struct VideoFrame;

// 媒体数据包
struct MediaPacket {
    QByteArray data;           // H.264/H.265/AAC 原始数据
    int64_t pts = 0;          // 显示时间戳
    bool isKeyFrame = false;  // 是否关键帧（仅视频有效）
    bool isVideo = false;     // 标记是否为视频帧
};

/**
 * @brief 插播视频源
 *
 * 继承 Source 接口，实现视频文件读取和解码
 * 功能：
 * - 读取视频文件
 * - 解码音视频帧
 * - 输出 VideoFrame 和 AudioFrame
 */
class MediaFileSource : public Source {
public:
    MediaFileSource(const std::string& id, std::shared_ptr<InsertFileItem> file_item);
    ~MediaFileSource() override;

    // ========== Source 接口 ==========
    bool initialize() override;
    bool start() override;
    bool stop() override;
    bool shutdown() override;

    std::shared_ptr<AudioFrame> get_audio_frame() override;
    std::shared_ptr<VideoFrame> get_video_frame() override;

    bool is_running() const override {
        return running_.load(std::memory_order_acquire);
    }

    std::string get_metadata() const override {
        return file_item_ ? file_item_->fileName.toStdString() : "";
    }

    // ========== 插播控制接口 ==========
    void pause() {
        paused_.store(true, std::memory_order_release);
    }

    void resume() {
        paused_.store(false, std::memory_order_release);
    }

    void seek(int64_t position_ms);

    // ========== 属性访问 ==========
    int get_width() const { return width_; }
    int get_height() const { return height_; }
    int64_t get_duration_ms() const { return duration_ms_; }
    std::shared_ptr<InsertFileItem> get_file_item() const { return file_item_; }
    int64_t get_current_position_ms() const { return current_position_ms_.load(); }
    bool is_finished() const { return finished_.load(); }

    // 信号（通过回调方式实现，因为Source不是QObject）
    using FrameReadyCallback = std::function<void(std::shared_ptr<VideoFrame>)>;
    using AudioReadyCallback = std::function<void(std::shared_ptr<AudioFrame>)>;
    using PlaybackFinishedCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const QString&)>;

    void set_frame_ready_callback(FrameReadyCallback callback) { frame_ready_callback_ = callback; }
    void set_audio_ready_callback(AudioReadyCallback callback) { audio_ready_callback_ = callback; }
    void set_playback_finished_callback(PlaybackFinishedCallback callback) { playback_finished_callback_ = callback; }
    void set_error_callback(ErrorCallback callback) { error_callback_ = callback; }

private:
    // ========== Reader 逻辑 ==========
    bool initializeReader();
    void readerThreadFunc();
    void shutdownReader();

    // ========== Scheduler 逻辑 ==========
    bool initializeDecoder();
    void schedulerThreadFunc();
    void shutdownDecoder();

    // ========== 解码辅助函数 ==========
    bool decodeVideoPacket(const AVPacket* packet);
    bool decodeAudioPacket(const AVPacket* packet);
    std::shared_ptr<VideoFrame> convertToVideoFrame(AVFrame* frame);
    std::shared_ptr<AudioFrame> convertToAudioFrame(AVFrame* frame);

    // ========== 数据成员 ==========
    std::shared_ptr<InsertFileItem> file_item_;

    // FFmpeg 上下文
    AVFormatContext* format_ctx_ = nullptr;
    AVCodecContext* video_codec_ctx_ = nullptr;
    AVCodecContext* audio_codec_ctx_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;       // 视频转码上下文 (YUV -> RGB)
    SwrContext* swr_ctx_ = nullptr;      // 音频重采样上下文
    AVFrame* video_frame_ = nullptr;
    AVFrame* audio_frame_ = nullptr;

    // 流索引
    int video_stream_idx_ = -1;
    int audio_stream_idx_ = -1;

    // 原始数据包队列
    std::queue<MediaPacket> video_packet_queue_;
    std::queue<MediaPacket> audio_packet_queue_;
    std::mutex video_packet_mutex_;
    std::mutex audio_packet_mutex_;
    std::condition_variable video_packet_cv_;
    std::condition_variable audio_packet_cv_;
    const size_t MAX_QUEUE_SIZE = 100;

    // 解码后的帧队列
    std::queue<std::shared_ptr<VideoFrame>> video_frame_queue_;
    std::queue<std::shared_ptr<AudioFrame>> audio_frame_queue_;
    std::mutex video_frame_mutex_;
    std::mutex audio_frame_mutex_;
    std::condition_variable video_frame_cv_;
    std::condition_variable audio_frame_cv_;
    const size_t MAX_FRAME_QUEUE_SIZE = 10;

    // 状态标志
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> reader_finished_{false};
    std::atomic<bool> finished_{false};
    std::atomic<int64_t> current_position_ms_{0};

    // 视频属性
    int width_ = 0;
    int height_ = 0;
    int64_t duration_ms_ = 0;
    double video_fps_ = 25.0;

    // 音频属性
    int audio_sample_rate_ = 48000;
    int audio_channels_ = 2;

    // 目标输出参数（与主直播匹配）
    int target_fps_ = 25;
    int target_width_ = 1280;
    int target_height_ = 720;
    int target_sample_rate_ = 48000;
    int target_channels_ = 2;

    // 线程
    std::thread reader_thread_;
    std::thread scheduler_thread_;

    // 回调函数
    FrameReadyCallback frame_ready_callback_;
    AudioReadyCallback audio_ready_callback_;
    PlaybackFinishedCallback playback_finished_callback_;
    ErrorCallback error_callback_;

    // 时间同步
    std::chrono::steady_clock::time_point start_time_;
    int64_t seek_position_ms_ = -1;
    std::atomic<bool> seek_requested_{false};
};

}  // namespace live_assistant

#endif // MEDIA_FILE_SOURCE_H
