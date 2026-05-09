#ifndef MEDIA_FILE_SOURCE_H
#define MEDIA_FILE_SOURCE_H

#include "media_pipeline/source.h"
#include "http/live_item.h"
#include <QObject>
#include <QImage>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <deque>

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
    int64_t pts = 0;          // 显示时间戳（流原始单位）
    int64_t pts_ms = 0;       // 显示时间戳（毫秒），用于 skip 阶段精确对齐音视频
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
 *
 * 架构：双线程模型
 * - Reader 线程：从文件读取压缩包到队列
 * - Scheduler 线程：解码包并按时间戳精确同步输出
 */
class MediaFileSource : public QObject, public Source {
    Q_OBJECT
public:
    MediaFileSource(const std::string& id, std::shared_ptr<InsertFileItem> file_item);
    ~MediaFileSource() override;

    // ========== Source 接口 ==========
    bool initialize() override;
    bool start() override;
    bool stop() override;
    bool shutdown() override;

    // ========== 快速重启（保留 FFmpeg 上下文，避免硬件解码器冷启动）==========
    // 用于暂停后恢复：seek 到指定位置（默认从头），flush 解码器，重启读取/调度线程
    // 比 shutdown() + initialize() + start() 快得多（无需重新打开文件和初始化解码器）
    // seek_position_ms = 0 从头播；> 0 从指定毫秒位置播
    bool seek_and_restart(int64_t seek_position_ms = 0);

    std::shared_ptr<AudioFrame> get_audio_frame() override;
    std::shared_ptr<VideoFrame> get_video_frame() override;

    bool is_running() const override {
        return running_.load(std::memory_order_acquire);
    }

    // seek_and_restart 之后、seek skip 完成之前返回 true（解码器在快速跳帧阶段）
    // 此期间 latest_frame_ 不更新、画面看起来冻结属于正常现象，不应响应用户暂停操作
    bool is_seeking() const {
        return seek_skip_target_ms_.load(std::memory_order_relaxed) > 0;
    }

    std::string get_metadata() const override {
        return file_item_ ? file_item_->fileName.toStdString() : "";
    }

    // ========== 循环播放控制 ==========
    void set_loop_enabled(bool enabled) {
        loop_enabled_ = enabled;
    }

    bool is_loop_enabled() const {
        return loop_enabled_;
    }

    // ========== 帧存储（用于 SceneManager 渲染，无锁）==========
    // Push a decoded frame (QImage) from decode pipeline for rendering
    void push_frame(const QImage& image);

    // 直接从 VideoFrame 推送帧（避免双重拷贝）
    void push_frame_with_raw_data(std::shared_ptr<VideoFrame> frame);

    // Get the latest pushed frame (thread-safe, no locking for read)
    // Returns null QImage if none available
    QImage get_latest_frame() const;

    // ========== 属性访问 ==========
    int get_width() const { return width_; }
    int get_height() const { return height_; }
    int64_t get_duration_ms() const { return duration_ms_; }
    std::shared_ptr<InsertFileItem> get_file_item() const { return file_item_; }
    int64_t get_current_position_ms() const { return current_position_ms_.load(); }
    bool is_finished() const { return finished_.load(); }

    // ========== 回调接口 ==========
    // 新增：带 PTS 的帧回调，用于同步
    using FrameReadyCallbackWithPTS = std::function<void(std::shared_ptr<VideoFrame>, int64_t pts_ms)>;
    using FrameReadyCallback = std::function<void(std::shared_ptr<VideoFrame>)>;
    using AudioReadyCallback = std::function<void(std::shared_ptr<AudioFrame>)>;
    using PlaybackFinishedCallback = std::function<void()>;

    void set_frame_ready_callback(FrameReadyCallback callback) { frame_ready_callback_ = callback; }
    // 新增：设置带 PTS 的回调（推荐使用）
    void set_frame_ready_callback_with_pts(FrameReadyCallbackWithPTS callback) { frame_ready_callback_with_pts_ = callback; }
    void set_audio_ready_callback(AudioReadyCallback callback) { audio_ready_callback_ = callback; }
    void set_playback_finished_callback(PlaybackFinishedCallback callback) { playback_finished_callback_ = callback; }

signals:
    // 解码后的视频帧信号（通过 Qt::QueuedConnection 自动在主线程执行）
    // 版本1：仅传 QImage（用于画布渲染）
    void videoFrameReady(const QImage& image, int64_t pts_ms);

    // 版本2：传 VideoFrame + PTS（用于画布渲染 + 推流编码）
    void videoFrameReadyWithFrame(std::shared_ptr<VideoFrame> frame, int64_t pts_ms);

    // 解码后的音频帧信号
    void audioFrameReady(std::shared_ptr<AudioFrame> frame);

    // 播放完成信号
    void playbackFinished();

public:
    // ========== 外部时间基准（用于与直播流同步）==========
    // 设置外部注入的时间基准（PTS 将基于此基准计算）
    // base_time_us: 外部时间基线（微秒），通常来自 MediaClock
    void set_external_time_base(int64_t base_time_us) {
        std::lock_guard<std::mutex> lock(external_time_base_mutex_);
        external_base_time_us_ = base_time_us;
    }

    // 同时设置时间基准和首帧文件PTS（原子操作，避免首帧PTS计算错误）
    // 用于在第一帧真正输出时由外部（bridge）调用，保证视频PTS与MediaClock严格对齐
    // base_time_us: 首帧对应的MediaClock时刻（微秒）
    // first_frame_file_pts_ms: 首帧在文件中的PTS（毫秒），用作相对偏移基准
    void set_external_time_base_with_first_pts(int64_t base_time_us, int64_t first_frame_file_pts_ms) {
        std::lock_guard<std::mutex> lock(external_time_base_mutex_);
        external_base_time_us_ = base_time_us;
        first_frame_pts_ms_.store(first_frame_file_pts_ms, std::memory_order_relaxed);
    }

    // 获取当前是否已设置外部时间基准
    bool has_external_time_base() const {
        std::lock_guard<std::mutex> lock(external_time_base_mutex_);
        return external_base_time_us_ > 0;
    }

    // 重置外部时间基准（停止插播时调用）
    void reset_external_time_base() {
        std::lock_guard<std::mutex> lock(external_time_base_mutex_);
        external_base_time_us_ = 0;
        first_frame_pts_ms_ = 0;
    }

private:
    // ========== Reader 线程逻辑 ==========
    bool initializeReader();
    void readerThreadFunc();
    void shutdownReader();

    // ========== Scheduler 线程逻辑 ==========
    bool initializeDecoder();
    void schedulerThreadFunc();
    void shutdownDecoder();

    // ========== 解码辅助函数 ==========
    bool decodeVideoPacket(const AVPacket* packet);
    bool decodeAudioPacket(const AVPacket* packet);
    std::shared_ptr<VideoFrame> convertToVideoFrame(AVFrame* frame);
    // 返回本次 decode 产生的所有完整固定样本帧（可能 0 或多个）
    std::vector<std::shared_ptr<AudioFrame>> convertToAudioFrame(AVFrame* frame);

    // ========== FFmpeg 上下文 ==========
    std::shared_ptr<InsertFileItem> file_item_;
    AVFormatContext* format_ctx_ = nullptr;
    AVCodecContext* video_codec_ctx_ = nullptr;
    AVCodecContext* audio_codec_ctx_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;       // 视频转码上下文 (YUV -> RGBA)
    SwrContext* swr_ctx_ = nullptr;      // 音频重采样上下文
    AVFrame* video_frame_ = nullptr;
    AVFrame* audio_frame_ = nullptr;

    // ========== 流索引 ==========
    int video_stream_idx_ = -1;
    int audio_stream_idx_ = -1;

    // ========== 原始数据包队列（Reader -> Scheduler） ==========
    std::queue<MediaPacket> video_packet_queue_;
    std::queue<MediaPacket> audio_packet_queue_;
    std::mutex video_packet_mutex_;
    std::mutex audio_packet_mutex_;
    std::condition_variable video_packet_cv_;
    std::condition_variable audio_packet_cv_;
    static const size_t MAX_QUEUE_SIZE = 100;

    // ========== 解码后的音频帧队列 ==========
    std::queue<std::shared_ptr<AudioFrame>> audio_frame_queue_;
    std::mutex audio_frame_mutex_;
    static const size_t MAX_FRAME_QUEUE_SIZE = 10;

    // ========== 视频帧缓冲队列（OBS风格：平滑抖动）==========
    // 使用 deque 而不是单一 latest_frame，可以保存多帧
    // 这样当解码或调度出现抖动时，渲染可以从队列中选择最合适的帧
    std::deque<std::shared_ptr<VideoFrame>> video_frame_queue_;
    std::mutex video_frame_mutex_;
    static const size_t MAX_VIDEO_FRAME_QUEUE_SIZE = 10;  // 保存10帧，约333ms@30fps

    // ========== 状态标志 ==========
    std::atomic<bool> running_{false};
    std::atomic<bool> reader_finished_{false};
    std::atomic<bool> finished_{false};
    std::atomic<bool> loop_enabled_{false};
    std::atomic<int64_t> current_position_ms_{0};

    // ========== 视频属性 ==========
    int width_ = 0;
    int height_ = 0;
    int64_t duration_ms_ = 0;
    double video_fps_ = 25.0;

    // ========== 音频属性 ==========
    int audio_sample_rate_ = 48000;
    int audio_channels_ = 2;

    // ========== 音频重采样缓冲区（实现固定样本输出）==========
    // WASAPI microphone capture arrives as 1024-sample chunks at 48 kHz. Keep
    // media chunks the same size so MIC+MEDIA mixing does not truncate or
    // overrun either source. Matching 1024 ensures the mixer fully blends both
    // sources each cycle without leaving silent gaps in the media audio.
    static const int TARGET_AUDIO_SAMPLES = 1024;
    std::vector<float> audio_resample_buffer_;       // 累积重采样数据
    size_t audio_resample_buffer_offset_ = 0;       // O(1) 读指针（避免 erase-from-front 的 O(N) 开销）
    int64_t audio_resample_timestamp_ms_ = 0;       // 缓冲区对应的时间戳
    bool audio_ts_initialized_ = false;              // 音频时间戳是否已初始化（允许首帧PTS=0）

    // ========== 解码器状态 ==========
    std::atomic<bool> need_decoder_flush_{false};   // 循环播放 seek 后需要 flush 解码器
    std::atomic<int64_t> seek_skip_target_ms_{0};   // 精确 seek：解码但跳过时间戳 < 此值的帧（keyframe 补偿）
    AVPixelFormat sws_src_fmt_ = AV_PIX_FMT_NONE;  // 当前 sws_ctx_ 对应的源格式，用于动态检查

    // ========== Reader 重同步（seek skip 后纠正 Reader 超前问题）==========
    // seek skip 期间 Scheduler 快速消费视频包 + 音频包被丢弃，Reader 在文件中
    // 向前跑得比视频目标位置快很多（可超前数秒）。skip 完成后须让 Reader seek
    // 回目标位置，否则音频数据来自文件的"未来"位置，造成音频超前视频数秒的感知延迟。
    std::atomic<bool> need_reader_resync_{false};
    std::atomic<int64_t> reader_resync_position_ms_{0};

    // resync 后 Reader 会 seek 回关键帧并重新设置 seek_skip_target_ms_ 进行短距离二次跳帧。
    // 此标记告知 Scheduler "这次 skip_done 是 resync 引起的二次跳帧"，不再触发第三次 resync。
    std::atomic<bool> post_resync_skip_active_{false};

    // ========== 目标输出参数（与主直播匹配）==========
    int target_width_ = 1280;
    int target_height_ = 720;
    int target_sample_rate_ = 48000;
    int target_channels_ = 2;

    // ========== 线程 ==========
    std::thread reader_thread_;
    std::thread scheduler_thread_;

    // ========== 回调函数 ==========
    FrameReadyCallbackWithPTS frame_ready_callback_with_pts_;  // 新增：带 PTS 的回调
    FrameReadyCallback frame_ready_callback_;
    AudioReadyCallback audio_ready_callback_;
    PlaybackFinishedCallback playback_finished_callback_;

    // ========== 外部时间基准（用于与直播流同步）==========
    mutable std::mutex external_time_base_mutex_;
    int64_t external_base_time_us_ = 0;  // 外部注入的时间基线
    std::atomic<int64_t> first_frame_pts_ms_{0};  // 第一帧的 PTS（用于计算相对偏移）- atomic 防止多线程重复初始化

    // ========== 帧存储（用于 SceneManager 渲染，无锁）==========
    mutable std::mutex latest_frame_mutex_;
    QImage latest_frame_;
};

}  // namespace live_assistant

#endif // MEDIA_FILE_SOURCE_H
