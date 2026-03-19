#pragma once

#include <string>

namespace live_assistant {

// 音频编码类型
enum class AudioCodecType {
    OPUS,
    AAC,
    UNKNOWN
};

// 音频编码模式
enum class AudioEncodingMode {
    CBR,  // 恒定比特率
    VBR,  // 可变比特率
    ABR   // 平均比特率
};

// AAC配置文件
enum class AACProfile {
    LC,    // 低复杂度
    HE,    // 高效 (AAC+)
    HEV2,  // 高效版本2 (AAC++)
    MAIN   // 主配置文件
};

// 视频编码类型
enum class VideoCodecType {
    H264,
    H265,
    UNKNOWN
};

// 视频编码模式
enum class VideoEncodingMode {
    CBR,  // 恒定比特率
    VBR,  // 可变比特率
    CQP   // 恒定量化参数 (constant QP)
};

// 视频编码预设 (适用于x264/x265)
enum class VideoEncodingPreset {
    ULTRAFAST,
    SUPERFAST,
    VERYFAST,
    FASTER,
    FAST,
    MEDIUM,
    SLOW,
    SLOWER,
    VERYSLOW,
    PLACEBO
};

// 硬件加速类型
enum class HWAccelerationType {
    NONE,     // 软件编码
    CUDA,     // NVIDIA CUDA
    AMF,      // AMD AMF
    QSV,      // Intel Quick Sync Video
    VAAPI,    // 视频加速API (Linux)
    VDPAU     // Unix视频解码和呈现API (Linux)
};

// 音频编码器配置
struct AudioEncoderConfig {
    // 基本参数
    AudioCodecType codec = AudioCodecType::AAC;  // 默认使用AAC以兼容
    int sample_rate = 48000;  // 默认采样率 (48kHz)
    int channels = 2;  // 默认立体声
    int bitrate = 128000;  // 默认比特率 (128kbps)
    AudioEncodingMode mode = AudioEncodingMode::CBR;  // 默认使用CBR
    
    // 音量控制 (0.0 - 1.0)
    float mic_volume = 0.8f;  // 麦克风音量，默认80%
    float speaker_volume = 0.8f;  // 扬声器音量，默认80%
    
    // Opus特定参数
    int opus_complexity = 9;  // 默认复杂度 (0-10, 10为最高)
    int opus_frame_size = 20;  // 默认帧大小 (毫秒) (2.5, 5, 10, 20, 40, 60)
    
    // AAC特定参数
    AACProfile aac_profile = AACProfile::LC;  // 默认使用LC配置文件
    
    // 默认构造函数
    AudioEncoderConfig() = default;
    
    // 带显式参数的构造函数
    AudioEncoderConfig(AudioCodecType codec, int sample_rate, int channels, int bitrate,
                       AudioEncodingMode mode = AudioEncodingMode::CBR)
        : codec(codec), sample_rate(sample_rate), channels(channels), bitrate(bitrate), mode(mode) {}
};

// 视频编码器配置
struct VideoEncoderConfig {
    // 基本参数
    VideoCodecType codec = VideoCodecType::H264;  // 默认使用H.264
    int width = 1280;  // 默认宽度 (720p)
    int height = 720;  // 默认高度 (720p)
    int fps = 30;  // 默认帧率
    int bitrate = 1500000;  // 默认比特率 (1500kbps) - 降低以减小文件大小
    // 默认使用 VBR，更友好地适应网络波动；用户可切换为 CBR 或 CQP
    VideoEncodingMode mode = VideoEncodingMode::VBR;
    
    // 高级参数
    // 默认GOP设置为2秒（帧数 = fps * 2）
    int gop = 60;  // 默认GOP大小（帧）
    // 最大瞬时码率（用于 VBV/CBR 限制），单位 bps。默认与 bitrate 相同。
    int max_bitrate = 2500000;
    float quality = 23.0f;  // 默认质量 (0-51, 数值越小质量越好)
    VideoEncodingPreset preset = VideoEncodingPreset::MEDIUM;  // 默认预设
    HWAccelerationType hw_accel = HWAccelerationType::NONE;  // 默认不强制指定硬编
    bool prefer_hw = true; // 默认优先尝试硬件编码（若可用）
    bool b_frames_enabled = false;  // 默认: B帧禁用
    
    // 默认构造函数
    VideoEncoderConfig() = default;
    
    // 带显式参数的构造函数
    VideoEncoderConfig(VideoCodecType codec, int width, int height, int fps, int bitrate,
                       VideoEncodingMode mode = VideoEncodingMode::VBR)
        : codec(codec), width(width), height(height), fps(fps), bitrate(bitrate), mode(mode),
          // 默认 2 秒 GOP
          gop(fps * 2), max_bitrate(bitrate), quality(23.0f), preset(VideoEncodingPreset::MEDIUM),
          hw_accel(HWAccelerationType::NONE), prefer_hw(true), b_frames_enabled(false) {}
};

} // namespace live_assistant
