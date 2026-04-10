#pragma once

#include <memory>
#include <mutex>
#include <deque>
#include <map>
#include <vector>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <QTimer>
#include <QObject>

#include "scene_manager/compositor.h"
#include "scene_manager/gpu_compositor.h"
#include "scene_manager/gpu_color_converter.h"
#include "scene_manager/gpu_texture_ref.h"
#include "encoder/encoder.h"
#include "stream_pusher/stream_pusher.h"
#include "common/media_clock.h"
#include "common/timestamp.h"
#include "common/video_frame_synchronizer.h"
#include "audio_engine/audio_resampler.h"
#include "audio_engine/audio_engine.h"  // 包含 AudioSourceType
#include "video_engine/video_engine.h"  // 包含 VideoFrame 定义
#include "media_pipeline/media_file_source.h"
#include "scene_manager/canvas.h"

namespace live_assistant {

// 🔧 编码前视频帧结构（用于编码队列）
struct PreEncodeVideoFrame {
    std::shared_ptr<VideoFrame> frame;
    // Phase 4 GPU path: 当 gpu_nv12_ref.is_valid() 时，优先走 encode_video_gpu_texture()
    GpuTextureRef gpu_nv12_ref;
    int64_t pts_ms;           // PTS（毫秒）
    int64_t wallclock_us;     // 壁挂钟时间（微秒）
    int64_t frame_count;      // 帧序号
};

// 音频缓冲区数据（照搬 OBS）
struct AudioBufferData {
    std::vector<uint8_t> data;
    int64_t timestamp_ms;
    uint32_t sample_rate = 48000;
    uint32_t channels = 2;
};

// 音频源信息（用于混音）
struct MixAudioSource {
    std::string source_id;
    AudioSourceType type;
    std::deque<AudioBufferData> buffer;  // 每个源独立的缓冲区
    std::unique_ptr<AudioResampler> resampler;  // 重采样器
    bool active = false;
    float volume = 1.0f;  // 音量控制（混音用）
};

class CompositorEncoderBridge : public QObject {
    Q_OBJECT

public:
    explicit CompositorEncoderBridge(QObject* parent = nullptr);
    ~CompositorEncoderBridge() override;

    // 设置组件
    void set_compositor(std::shared_ptr<Compositor> compositor);
    void set_encoder(std::shared_ptr<Encoder> encoder);
    // GPU 管线接线（Phase 2-4）：设置后自动启用 GPU 路径
    void set_gpu_compositor(std::shared_ptr<GpuCompositor> gpu_compositor);
    void set_gpu_color_converter(std::shared_ptr<GpuColorConverter> gpu_color_converter);
    void set_stream_pusher(std::shared_ptr<StreamPusher> stream_pusher);
    void set_audio_engine(std::shared_ptr<class AudioEngine> audio_engine);
    // 静音回退开关（当麦克风不可用时启用静音帧）
    void set_silent_audio(bool enable);

    // 控制
    void start(int fps = 30);
    void stop();
    bool is_running() const;

    // 推流控制
    bool start_streaming(const std::string& url);
    void stop_streaming();
    bool is_streaming() const;

    // 配置
    void set_fps(int fps);
    void set_resolution(int width, int height);

    // 降级策略
    void enable_adaptive_quality(bool enable);
    void set_quality_thresholds(int min_fps, int max_fps);

    // 🔧 音频源管理（照搬 OBS 的多路音频支持）
    void register_audio_source(const std::string& source_id, AudioSourceType type);
    void unregister_audio_source(const std::string& source_id);
    void set_audio_source_volume(const std::string& source_id, float volume);
    bool push_audio_data(const std::string& source_id, const QByteArray& data, 
                         int64_t timestamp, uint32_t sample_rate = 48000);

    // 设置 CanvasRenderer 用于推流捕获（回退方案：不使用 Compositor）
    void set_canvas_renderer(std::shared_ptr<CanvasRenderer> renderer, std::shared_ptr<Scene> scene);

    // ═══════════════════════════════════════════════════════════════
    // 🔧 插播视频帧同步（新增）
    // ═══════════════════════════════════════════════════════════════
    // 附加插播视频源（注入时间基准）
    void attach_insert_video_source(MediaFileSource* source);
    // 分离插播视频源（清除时间基准）
    void detach_insert_video_source(MediaFileSource* source);
    // 从同步器获取插播视频帧（非阻塞）
    bool pop_insert_video_frame(SyncedVideoFrame& out_frame);
    // 推送插播视频帧到同步器（用于编码）
    bool push_insert_video_frame(std::shared_ptr<VideoFrame> frame, int64_t pts_ms);

signals:
    void frame_encoded(const std::vector<uint8_t>& data);
    void quality_degraded(const QString& reason);
    void quality_restored();

    // 推流状态信号
    void streaming_started();
    void streaming_stopped();
    void streaming_error(const QString& error);

private slots:
    void on_encode_timer();
    // 处理音频引擎的原始数据（提交到混音器）
    void on_audio_data_ready(const QByteArray& data, int64_t timestamp);
    // 处理编码后的音频数据（推送到流）
    void on_audio_encoded(const QByteArray& data, int64_t timestamp);

private:
    void encode_and_push_frame();
    std::shared_ptr<VideoFrame> capture_compositor_frame();
    std::shared_ptr<VideoFrame> convert_qimage_to_video_frame(const QImage& img);

    // 音频处理（照搬 OBS）
    void process_audio_buffer();  // 音频缓冲区处理
    void generate_silent_audio(std::vector<int16_t>& output, uint32_t frames);  // 生成静音帧
    bool validate_audio_format(uint32_t sample_rate, uint32_t channels, uint32_t bits_per_sample);

    // 🔧 降级策略处理
    void check_and_adjust_quality(int64_t encode_time_ms);

    // 组件
    std::shared_ptr<Compositor> compositor_;
    // GPU 路径（Phase 2-4），可选——未设置时回退 CPU 路径
    std::shared_ptr<GpuCompositor>    gpu_compositor_;
    std::shared_ptr<GpuColorConverter> gpu_color_converter_;
    std::shared_ptr<CanvasRenderer> canvas_renderer_;  // 回退方案：用于推流捕获
    std::shared_ptr<Scene> current_scene_;            // 当前场景（用于 CanvasRenderer 渲染）
    std::shared_ptr<Encoder> encoder_;
    std::shared_ptr<StreamPusher> stream_pusher_;
    std::shared_ptr<AudioEngine> audio_engine_;


    // 工作线程定时器（避免阻塞主线程）
    std::unique_ptr<std::thread> capture_thread_;
    std::atomic<bool> capture_thread_stop_{false};
    // 捕获线程投递给主线程的帧是否还未被处理。
    // 防止主线程繁忙时多个 QueuedConnection 积压，导致帧突发/PTS 抖动。
    std::atomic<bool> frame_pending_{false};
    std::chrono::steady_clock::time_point last_capture_time_;
    std::atomic<int> capture_fps_{30};

    // 配置
    int fps_ = 30;
    int width_ = 1920;
    int height_ = 1080;

    // 状态（running_ 跨线程访问，必须原子）
    std::atomic<bool> running_{false};
    std::atomic<bool> streaming_{false};
    std::string stream_url_;
    MediaClock media_clock_;
    bool silent_audio_enabled_ = false;

    int64_t video_frame_count_ = 0;

    // ═══════════════════════════════════════════════════════════════
    // 🔧 异步编码架构：编码队列 + 独立编码线程
    // ═══════════════════════════════════════════════════════════════
    
    // 启动/停止编码线程池
    void start_encoder_threads();
    void stop_encoder_threads();
    
    // 捕获帧并加入编码队列（定时器回调）
    void capture_and_queue_frame();
    
    // 编码前视频帧队列（线程安全）
    // 🔧 编码队列大小：10帧（约333ms @ 30fps）
    // 提供足够缓冲避免因编码波动导致的丢帧
    static constexpr size_t MAX_ENCODE_QUEUE_SIZE = 10;
    std::deque<PreEncodeVideoFrame> pre_encode_queue_;
    std::mutex encode_queue_mutex_;
    std::condition_variable encode_queue_cv_;
    
    // 编码线程池（目前只用1个线程，保证 PTS 顺序）
    static constexpr int MAX_ENCODER_THREADS = 1;
    std::vector<std::thread> encoder_threads_;
    std::atomic<bool> encoder_threads_running_{false};
    
    // 编码线程工作函数
    void encoder_thread_func(int thread_id);
    
    // 将帧加入编码队列（满了丢弃最旧的）
    void push_to_encode_queue(const PreEncodeVideoFrame& frame);
    
    // 从编码队列取帧（阻塞等待）
    bool pop_from_encode_queue(PreEncodeVideoFrame& frame);

    // 🔧 多路音频系统（照搬 OBS obs-output.c）
    std::mutex audio_sources_mutex_;
    std::map<std::string, std::unique_ptr<MixAudioSource>> audio_sources_;  // 多路音频源
    
    // 混音输出缓冲区（float 格式，与 OBS 一致）
    std::mutex mix_buffer_mutex_;
    std::vector<float> mix_accumulated_data_;  // 累积的混音数据
    uint64_t total_audio_samples_ = 0;  // 累计音频样本数
    int64_t audio_start_ts_ = 0;        // 音频开始时间戳
    bool audio_started_ = false;
    
    // 🔧 音频时间戳偏移（微秒），用于计算每帧的 PTS
    int64_t audio_timestamp_offset_us_ = 0;
    
    // 🔧 第一帧音频的时间戳偏移（毫秒），用于让时间戳从0开始
    int64_t first_audio_timestamp_ms_ = -1;

    // 🔧 记录 media_clock 在推流开始时的起始时间戳（微秒）
    int64_t media_clock_start_us_ = 0;

    // steady_clock absolute microseconds when start_streaming() was called.
    // Used to convert the mixer's emit-time steady_clock timestamp back to
    // bridge-relative PTS even when the slot runs later (QueuedConnection).
    int64_t streaming_start_steady_us_ = 0;

    // Audio PTS alignment: offset (ms) subtracted from media_clock to align
    // first audio packet with first video packet. INT64_MIN = not yet computed.
    // Protected by audio_pts_mutex_ because on_audio_data_ready now runs in the
    // mixer thread (DirectConnection) while first_video_pts_ms_ is written by
    // the video capture thread.
    std::mutex audio_pts_mutex_;
    int64_t audio_pts_clock_offset_ms_ = INT64_MIN;

    // SWS 上下文缓存（用于 RGBA 到 NV12 转换）
    void* sws_context_ = nullptr;
    int cached_width_ = 0;
    int cached_height_ = 0;

    // SWS 上下文管理
    void initialize_opengl_context();
    void* get_or_create_sws_context(int src_width, int src_height);
    void release_sws_context();

    // 合成器帧回调
    void on_compositor_frame_ready();

    // 🔧 视频时间戳
    int64_t video_start_ts_ = 0;
    bool video_started_ = false;
    
    // 🔧 OBS 风格 PTS 偏移归零（确保第一帧 PTS=0）
    int64_t first_video_pts_ms_ = -1;      // 第一帧音/视频的 PTS（毫秒），音视频共用
    bool streaming_pts_initialized_ = false;  // 推流 PTS 是否已初始化

    // 🔧 视频 PTS 帧计数器基准：确保每个编码帧 PTS 等间隔
    // 不使用 media_clock 实时值（受 QTimer 抖动影响），而是用帧号 × 帧时长推算
    int64_t video_pts_base_us_ = -1;       // 首帧的 media_clock（微秒），用于诊断
    int64_t video_pts_initial_frame_ = -1;  // 首帧的 video_frame_count_，用于帧号偏移

    // 音频时钟漂移监控（仅诊断，不修正 PTS）
    int64_t audio_total_samples_received_ = 0;  // 从声卡实际收到的总采样数
    int64_t last_drift_check_ms_ = 0;           // 上次漂移检查的 media_clock 时间
    static constexpr int64_t DRIFT_CHECK_INTERVAL_MS = 10000;   // 每 10 秒检测一次
    static constexpr int64_t DRIFT_THRESHOLD_US = 40000;        // 日志输出阈值

    // 🔧 on_audio_encoded 诊断计数（成员变量，替换 static 本地变量，每次推流重置）
    int64_t diag_first_audio_pts_ = -1;
    int diag_audio_count_ = 0;

    // ═══════════════════════════════════════════════════════════════
    // 🔧 插播视频帧同步器（用于与直播流时间同步）
    // ═══════════════════════════════════════════════════════════════
    std::unique_ptr<VideoFrameSynchronizer> insert_video_synchronizer_;

    // GPU 路径：capture_compositor_frame() → capture_and_queue_frame() 传递 NV12 纹理引用
    // 只在捕获线程访问，无需加锁
    GpuTextureRef pending_gpu_nv12_ref_;

    // GPU NV12 快照纹理：USAGE_DEFAULT + BindFlags=0，用于 GPU→GPU 拷贝（无 staging detile 崩溃）
    // GpuColorConverter 输出（BIND_RENDER_TARGET）→ 快照（DEFAULT）→ QSV 编码纹理
    winrt::com_ptr<ID3D11Texture2D> nv12_snapshot_;

    // GPU 编码回退用黑帧（GPU 路径失败时 CPU 回退用，仅分配一次）
    std::shared_ptr<VideoFrame> black_frame_cache_;

    // 确保快照纹理存在且尺寸匹配，返回 false 表示创建失败
    bool ensure_nv12_snapshot(int w, int h);

    // 线程安全：保护状态变量的互斥锁
    mutable std::mutex state_mutex_;

    // 🔧 性能监控
    struct PerformanceStats {
        int64_t video_frames_encoded = 0;
        int64_t audio_frames_encoded = 0;
        int64_t video_frames_dropped = 0;
        int64_t audio_frames_dropped = 0;
        double avg_encode_time_ms = 0.0;
        double max_encode_time_ms = 0.0;
        int64_t last_log_time = 0;
        // 🔧 降级策略用
        std::deque<int64_t> encode_times_;  // 最近编码时间记录
        static constexpr size_t MAX_ENCODE_TIME_HISTORY = 30;
    } perf_stats_;

    // 降级策略
    bool adaptive_quality_enabled_ = true;
    int min_fps_threshold_ = 20;
    int max_fps_threshold_ = 25;
    bool quality_degraded_ = false;
    int original_fps_ = 30;
    int degraded_fps_ = 15;
};

} // namespace live_assistant
