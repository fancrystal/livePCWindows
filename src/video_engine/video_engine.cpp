#include "video_engine/video_engine.h"
#include "scene_manager/scene_manager.h"
#include "common/log.h"
#include "common/error.h"

#include <chrono>
#include <thread>

// FFmpeg includes
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavdevice/avdevice.h>
#include <libavutil/imgutils.h>
}

// OpenCV includes - placed after log.h to avoid namespace pollution
#include <opencv2/opencv.hpp>
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/imgproc.hpp>

namespace live_assistant {

VideoEngine::VideoEngine() {
    // 初始化FFmpeg
    avdevice_register_all();
    ffmpeg_initialized_ = true;

    // 初始化OpenCV
    opencv_initialized_ = true;

    // 默认使用OpenCV捕获模式
    capture_mode_ = CaptureMode::OPENCV;

    LOG_INFO("Initialized VideoEngine with FFmpeg and OpenCV support, default mode: OPENCV");
}

VideoEngine::~VideoEngine() {
    shutdown();
    LOG_INFO("VideoEngine destroyed");
}

bool VideoEngine::initialize(int width, int height, int fps) {
    // 检查参数有效性
    if (width <= 0 || height <= 0 || fps <= 0) {
        LOG_ERROR("Invalid initialization parameters: width=" + std::to_string(width) +
                  ", height=" + std::to_string(height) +
                  ", fps=" + std::to_string(fps));
        return false;
    }

    output_width_ = width;
    output_height_ = height;
    fps_ = fps;

    // 初始化latest_frame_为一个空帧，确保它始终是有效的
    try {
        latest_frame_ = std::make_shared<VideoFrame>(width, height);
        if (!latest_frame_) {
            LOG_ERROR("Failed to create VideoFrame");
            return false;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Exception when creating VideoFrame: " + std::string(e.what()));
        return false;
    }

    LOG_INFO("VideoEngine initialized with resolution " +
              std::to_string(width) + "x" + std::to_string(height) +
              " at " + std::to_string(fps) + "fps");

    return true;
}

bool VideoEngine::shutdown() {
    if (is_capturing_) {
        stop_capture();
    }

    release_ffmpeg();
    release_opencv();

    LOG_INFO("VideoEngine shutdown");
    return true;
}

bool VideoEngine::initialize_ffmpeg() {
    if (!ffmpeg_initialized_) {
        LOG_ERROR("FFmpeg not initialized");
        return false;
    }
    
    // 如果已经初始化，先释放
    release_ffmpeg();
    
    // 创建AVFormatContext
    AVFormatContext* fmt_ctx = avformat_alloc_context();
    if (!fmt_ctx) {
        LOG_ERROR("Failed to allocate AVFormatContext");
        return false;
    }
    
    // 设置输入格式为dshow
    const AVInputFormat* input_format = av_find_input_format("dshow");
    if (!input_format) {
        LOG_ERROR("Failed to find dshow input format");
        avformat_free_context(fmt_ctx);
        return false;
    }
    
    // 准备设备名称字符串
    std::string device_url = "video=" + selected_camera_;
    
    // 打开摄像头设备
    AVDictionary* options = nullptr;
    
    // 设置捕获分辨率和帧率
    av_dict_set(&options, "video_size", (std::to_string(output_width_) + "x" + std::to_string(output_height_)).c_str(), 0);
    av_dict_set(&options, "framerate", std::to_string(fps_).c_str(), 0);
    
    if (avformat_open_input(&fmt_ctx, device_url.c_str(), const_cast<AVInputFormat*>(input_format), &options) < 0) {
            LOG_ERROR("Failed to open camera device: " + selected_camera_);
            avformat_free_context(fmt_ctx);
            av_dict_free(&options);
            return false;
        }
    
    av_dict_free(&options);
    
    // Find stream information
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        LOG_ERROR("Failed to find stream information");
        avformat_close_input(&fmt_ctx);
        return false;
    }
    
    // Find video stream
    int video_stream_idx = -1;
    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }
    
    if (video_stream_idx == -1) {
        LOG_ERROR("No video stream found");
        avformat_close_input(&fmt_ctx);
        return false;
    }
    
    // Find decoder
    AVCodecParameters* codec_par = fmt_ctx->streams[video_stream_idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codec_par->codec_id);
    if (!codec) {
        LOG_ERROR("Failed to find decoder");
        avformat_close_input(&fmt_ctx);
        return false;
    }
    
    // Create AVCodecContext
    AVCodecContext* codec_ctx = avcodec_alloc_context3(const_cast<AVCodec*>(codec));
    if (!codec_ctx) {
        LOG_ERROR("Failed to allocate AVCodecContext");
        avformat_close_input(&fmt_ctx);
        return false;
    }
    
    // Copy codec parameters to codec context
    if (avcodec_parameters_to_context(codec_ctx, codec_par) < 0) {
        LOG_ERROR("Failed to copy codec parameters");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return false;
    }
    
    // Open codec
    if (avcodec_open2(codec_ctx, const_cast<AVCodec*>(codec), nullptr) < 0) {
        LOG_ERROR("Failed to open codec");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return false;
    }
    
    // Set FFmpeg context pointers
    av_format_context_ = fmt_ctx;
    av_codec_context_ = codec_ctx;
    video_stream_index_ = video_stream_idx;
    
    LOG_INFO("FFmpeg initialized successfully for camera: " + selected_camera_);
    
    return true;
}

void VideoEngine::release_ffmpeg() {
    try {
        if (sws_context_) {
            sws_freeContext(static_cast<SwsContext*>(sws_context_));
            sws_context_ = nullptr;
        }

        if (av_codec_context_) {
            AVCodecContext* codec_ctx = static_cast<AVCodecContext*>(av_codec_context_);
            avcodec_close(codec_ctx);
            avcodec_free_context(&codec_ctx);
            av_codec_context_ = nullptr;
        }

        if (av_format_context_) {
            AVFormatContext* fmt_ctx = static_cast<AVFormatContext*>(av_format_context_);
            avformat_close_input(&fmt_ctx);
            av_format_context_ = nullptr;
        }

        video_stream_index_ = -1;
        ffmpeg_initialized_ = false;
        LOG_INFO("FFmpeg resources released successfully");
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during FFmpeg resource release: " + std::string(e.what()));
    }
}

std::shared_ptr<VideoFrame> VideoEngine::get_latest_frame() {
    std::unique_lock<std::mutex> lock(frame_mutex_);
    return latest_frame_;
}

std::vector<std::string> VideoEngine::get_available_cameras() {
    auto choices = get_available_camera_choices();
    std::vector<std::string> names;
    names.reserve(choices.size());
    for (const auto& choice : choices) {
        names.push_back(choice.display_name);
    }
    return names;
}

std::vector<VideoEngine::CameraChoice> VideoEngine::get_available_camera_choices() {
    std::vector<CameraChoice> choices;

    // 虚拟摄像头过滤关键词列表（参考OBS和常见虚拟摄像头名称）
    static const char* virtual_camera_keywords[] = {
        // 英文关键词
        "virtual",           // 通用虚拟摄像头关键词
        "Virtual Camera",    // OBS Virtual Camera
        "DirectShow Show",   // DirectShow Show
        "ManyCam",
        "Splitcam",
        "CamTwist",
        "Webcam Simulator",
        "Siphon",
        "Blackmagic",
        "vcam",
        "cam4k",
        "cam3k",
        "v4l2loopback",
        // 中文关键词
        "虚拟摄像头",
        "虚拟相机",
        // 其他已知问题设备
        "TikTok",            // TikTok Live Studio Virtual Camera
        "lsvcam",            // TikTok Live Studio
        "StreamDeck",        // StreamDeck有虚拟摄像头功能
        "ToDesk",            // ToDesk 远程桌面虚拟摄像头
        "OMEN Cam",          // OMEN Cam & Voice 虚拟摄像头
    };

    auto is_virtual_camera = [](const char* description) -> bool {
        if (!description) return false;
        std::string desc_lower = description;
        // 转换为小写进行比较
        for (char& c : desc_lower) {
            c = static_cast<char>(tolower(c));
        }
        for (const char* keyword : virtual_camera_keywords) {
            std::string kw_lower = keyword;
            for (char& c : kw_lower) {
                c = static_cast<char>(tolower(c));
            }
            if (desc_lower.find(kw_lower) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    if (ffmpeg_initialized_) {
        AVDeviceInfoList* device_list = nullptr;
        const AVInputFormat* input_format = av_find_input_format("dshow");
        if (input_format && avdevice_list_input_sources(const_cast<AVInputFormat*>(input_format), "video", nullptr, &device_list) >= 0) {
                LOG_INFO("Found " + std::to_string(device_list->nb_devices) + " video devices via FFmpeg");
                for (int i = 0; i < device_list->nb_devices; i++) {
                if (auto* device = static_cast<AVDeviceInfo*>(device_list->devices[i])) {
                    // Filter out non-video devices that dshow sometimes lists
                    if (strstr(device->device_description, "麦克风") != nullptr || strstr(device->device_description, "Microphone") != nullptr) {
                        continue;
                    }

                    // Filter out virtual cameras
                    if (is_virtual_camera(device->device_description)) {
                        LOG_INFO("Filtering out virtual camera: " + std::string(device->device_description ? device->device_description : "Unknown"));
                        continue;
                    }

                    CameraChoice choice;
                    // display_name 用于 UI 显示
                    choice.display_name = device->device_description ? device->device_description : "Unknown Device";
                    // dshow_name 用于 FFmpeg 打开摄像头（需要用 Friendly Name，不是 DirectShow 路径）
                    // FFmpeg dshow 格式: video="Friendly Name"
                    choice.dshow_name = device->device_description ? device->device_description : "";

                    if (!choice.dshow_name.empty()) {
                        choices.push_back(choice);
                        LOG_INFO("Found camera: '" + choice.display_name + "' (dshow_name for FFmpeg: " + choice.dshow_name + ")");
                    }
                }
            }
                avdevice_free_list_devices(&device_list);
        }
    }

    // Fallback if FFmpeg fails
    if (choices.empty() && opencv_initialized_) {
        LOG_WARNING("FFmpeg enumeration failed or found no cameras, falling back to OpenCV index probing.");
        for (int i = 0; i < 10; ++i) {
            cv::VideoCapture cap;
            if (cap.open(i, cv::CAP_DSHOW)) {
                cv::Mat test_frame;
                if (cap.read(test_frame) && !test_frame.empty()) {
                    CameraChoice choice;
                    choice.display_name = "Camera " + std::to_string(i);
                    choice.dshow_name = std::to_string(i);  // OpenCV 用索引
                    choice.opencv_index = i;
                    choices.push_back(choice);
                    }
                    cap.release();
            }
        }
    } else {
        // FFmpeg 枚举成功，为每个摄像头设置默认 OpenCV 索引
        for (auto& choice : choices) {
            choice.opencv_index = 0;  // 默认使用第一个摄像头
        }
    }

    return choices;
}

bool VideoEngine::select_camera(const std::string& camera_description) {
    // Directly use the input as the camera device path/name
    // The camera_devices_ list is not populated, so the lookup logic was ineffective
    // This allows selection by: dshow device name, OpenCV camera index, or any identifier
    selected_camera_ = camera_description;
    LOG_INFO("Selected camera: " + camera_description);
    return true;
}

bool VideoEngine::set_capture_mode(CaptureMode mode) {
    if (is_capturing_) {
        LOG_ERROR("Cannot change capture mode while capturing");
        return false;
    }
    
    capture_mode_ = mode;
    
    std::string mode_str;
    switch (mode) {
        case CaptureMode::FFMPEG:
            mode_str = "FFMPEG";
            break;
        case CaptureMode::OPENCV:
            mode_str = "OPENCV";
            break;
    }
    
    LOG_INFO("Set capture mode to: " + mode_str);
    return true;
}

CaptureMode VideoEngine::get_capture_mode() const {
    return capture_mode_;
}

bool VideoEngine::set_camera_resolution(const std::string& resolution) {
    camera_resolution_ = resolution;
    LOG_INFO("Set camera resolution to: " + resolution);
    return true;
}

bool VideoEngine::set_camera_fps(int fps) {
    camera_fps_ = fps;
    LOG_INFO("Set camera fps to: " + std::to_string(fps));
    return true;
}

bool VideoEngine::set_camera_pixel_format(const std::string& pixel_format) {
    camera_pixel_format_ = pixel_format;
    LOG_INFO("Set camera pixel format to: " + pixel_format);
    return true;
}

bool VideoEngine::set_camera_mirror(bool mirror) {
    camera_mirror_ = mirror;
    LOG_INFO("Set camera mirror to: " + std::string(mirror ? "enabled" : "disabled"));
    return true;
}

std::string VideoEngine::get_camera_resolution() const {
    return camera_resolution_;
}

int VideoEngine::get_camera_fps() const {
    return camera_fps_;
}

std::string VideoEngine::get_camera_pixel_format() const {
    return camera_pixel_format_;
}

bool VideoEngine::get_camera_mirror() const {
    return camera_mirror_;
}

bool VideoEngine::start_capture() {
    if (is_capturing_) {
        LOG_WARNING("Video capture already started");
        return true;
    }
    
    bool result = true;
    
    // Initialize according to capture mode
    if (capture_mode_ == CaptureMode::FFMPEG) {
        if (selected_camera_.empty()) {
            LOG_ERROR("No camera selected");
            return false;
        }
        
        result = initialize_ffmpeg();
        if (!result) {
            LOG_ERROR("Failed to initialize FFmpeg for camera capture");
            return false;
        }
    } else if (capture_mode_ == CaptureMode::OPENCV) {
        if (selected_camera_.empty()) {
            LOG_ERROR("No camera selected");
            return false;
        }
        
        result = initialize_opencv();
        if (!result) {
            LOG_ERROR("Failed to initialize OpenCV for camera capture");
            return false;
        }
    }
    
    // Start capture thread
    stop_thread_ = false;
    capture_thread_ = std::thread(&VideoEngine::capture_thread_func, this);
    
    is_capturing_ = true;
    
    LOG_INFO("Started video capture from camera: " + selected_camera_ + 
              " using " + (capture_mode_ == CaptureMode::FFMPEG ? "FFMPEG" : "OPENCV") + " mode");
    
    return true;
}

bool VideoEngine::stop_capture() {
    if (!is_capturing_) {
        LOG_WARNING("Video capture already stopped");
        return true;
    }
    
    // Signal thread to stop
    stop_thread_ = true;
    
    // Wait for thread to exit
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    
    // Release resources according to capture mode
    if (capture_mode_ == CaptureMode::FFMPEG) {
        release_ffmpeg();
    } else if (capture_mode_ == CaptureMode::OPENCV) {
        release_opencv();
    }
    
    is_capturing_ = false;
    
    LOG_INFO("Stopped video capture");
    
    // Clear latest frame to avoid showing a frozen image after stopping
    {
        std::unique_lock<std::mutex> lock(frame_mutex_);
        latest_frame_.reset();
    }

    return true;
}

void VideoEngine::set_current_scene(std::shared_ptr<Scene> scene) {
    current_scene_ = scene;
    if (scene) {
        LOG_INFO("Set current scene to: " + scene->get_name());
    }
}

// Render a frame from the current scene (interface method)
std::shared_ptr<VideoFrame> VideoEngine::render_frame() {
    // 如果没有当前场景，直接返回最新捕获的帧
    if (!current_scene_) {
        // LOG_INFO("No current scene, returning latest captured frame");
        return get_latest_frame();
    }

    // 获取场景中的所有场景项
    std::vector<std::shared_ptr<SceneItem>> scene_items = current_scene_->get_all_scene_items();

    // 如果场景中没有任何项，直接返回最新捕获的帧
    if (scene_items.empty() && rand() % 3 == 0) {
        // LOG_INFO("Current scene has no items, returning latest captured frame");
        return get_latest_frame();
    }

    // 获取最新捕获的帧作为背景
    auto rendered_frame = get_latest_frame();

    // 每帧都打印会导致编码线程卡顿，仅调试时打开
    // LOG_INFO("Rendering scene: " + current_scene_->get_name() + ", items: " + std::to_string(scene_items.size()));
    
    // TODO: 实现真正的场景渲染逻辑，包括:
    // 1. 按顺序渲染每个可见的SceneItem
    // 2. 应用变换（位置、大小、旋转、透明度）
    // 3. 将渲染结果合并为一个VideoFrame
    
    return rendered_frame;
}

int VideoEngine::get_output_width() const {
    return output_width_;
}

int VideoEngine::get_output_height() const {
    return output_height_;
}

void VideoEngine::capture_thread_func() {
    std::string mode_str;
    switch (capture_mode_) {
        case CaptureMode::FFMPEG:
            mode_str = "FFMPEG";
            break;
        case CaptureMode::OPENCV:
            mode_str = "OPENCV";
            break;
        default:
            mode_str = "UNKNOWN";
            break;
    }
    
    LOG_INFO("Capture thread started using " + mode_str + " mode");
    
    // Call the appropriate capture thread function based on the selected mode
    if (capture_mode_ == CaptureMode::FFMPEG) {
        ffmpeg_capture_thread_func();
    } else if (capture_mode_ == CaptureMode::OPENCV) {
        opencv_capture_thread_func();
    }
    
    LOG_INFO("Capture thread stopping");
}

void VideoEngine::ffmpeg_capture_thread_func() {
    if (!av_format_context_ || !av_codec_context_) {
        LOG_ERROR("Invalid FFmpeg context in capture thread");
        return;
    }
    
    AVFormatContext* fmt_ctx = static_cast<AVFormatContext*>(av_format_context_);
    AVCodecContext* codec_ctx = static_cast<AVCodecContext*>(av_codec_context_);
    
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* rgb_frame = av_frame_alloc();
    
    if (!packet || !frame || !rgb_frame) {
        LOG_ERROR("Failed to allocate FFmpeg frames");
        av_frame_free(&rgb_frame);
        av_frame_free(&frame);
        av_packet_free(&packet);
        return;
    }
    
    // Allocate buffer for RGBA frame
    int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, output_width_, output_height_, 1);
    uint8_t* buffer = static_cast<uint8_t*>(av_malloc(num_bytes * sizeof(uint8_t)));
    if (!buffer) {
        LOG_ERROR("Failed to allocate buffer for RGBA frame");
        av_frame_free(&rgb_frame);
        av_frame_free(&frame);
        av_packet_free(&packet);
        return;
    }
    
    // Assign buffer to RGB frame
    av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, buffer, AV_PIX_FMT_RGBA, output_width_, output_height_, 1);
    
    int retry_count = 0;
    const int max_retries = 5;
    
    while (!stop_thread_) {
        // Read frame from camera
        if (av_read_frame(fmt_ctx, packet) < 0) {
            // End of stream or error, try to restart
            LOG_WARNING("End of stream or error reading frame, retrying...");
            retry_count++;
            
            // Check if we should stop
            if (stop_thread_) {
                break;
            }
            
            // Limit retry attempts to avoid infinite loop
            if (retry_count > max_retries) {
                LOG_ERROR("Max retry attempts reached, stopping capture");
                break;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            av_packet_unref(packet);
            continue;
        }
        
        // Reset retry count on successful frame read
        retry_count = 0;
        
        // Check if it's the video stream
        if (packet->stream_index == video_stream_index_) {
            // Send packet to decoder
            if (avcodec_send_packet(codec_ctx, packet) < 0) {
                LOG_ERROR("Failed to send packet to decoder");
                av_packet_unref(packet);
                continue;
            }
            
            // Receive decoded frame
            while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                // Check if we should stop
                if (stop_thread_) {
                    break;
                }
                
                // Create or update sws context if needed
                if (!sws_context_) {
                    sws_context_ = sws_getContext(
                        frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                        output_width_, output_height_, AV_PIX_FMT_RGBA,
                        SWS_BILINEAR, nullptr, nullptr, nullptr);
                    
                    if (!sws_context_) {
                        LOG_ERROR("Failed to create sws context");
                        break;
                    }
                }
                
                // Convert frame to RGBA
                sws_scale(
                    static_cast<SwsContext*>(sws_context_),
                    frame->data,
                    frame->linesize,
                    0,
                    frame->height,
                    rgb_frame->data,
                    rgb_frame->linesize);
                
                // Create VideoFrame object with timestamp
                auto video_frame = std::make_shared<VideoFrame>(output_width_, output_height_);
                
                // Copy RGBA data to VideoFrame
                int frame_size = output_width_ * output_height_ * 4;
                std::memcpy(video_frame->data.get(), buffer, frame_size);
                
                // Set timestamp (use current time in milliseconds)
                auto now = std::chrono::system_clock::now();
                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
                video_frame->timestamp_ms = now_ms.count();
                
                // Update latest frame with thread safety
                {
                    std::unique_lock<std::mutex> lock(frame_mutex_);
                    latest_frame_ = std::move(video_frame);
                }
                
                // Notify waiting threads
                frame_cv_.notify_one();
            }
        }
        
        // Unref packet
        av_packet_unref(packet);
    }
    
    // Cleanup
    av_freep(&buffer);
    av_frame_free(&rgb_frame);
    av_frame_free(&frame);
    av_packet_free(&packet);
}

void VideoEngine::opencv_capture_thread_func() {
    if (!cv_video_capture_) {
        LOG_ERROR("Invalid OpenCV VideoCapture in capture thread");
        return;
    }

    cv::VideoCapture* cap = static_cast<cv::VideoCapture*>(cv_video_capture_);
    cv::Mat frame, rgba_frame;

    int retry_count = 0;
    const int max_retries = 5;

    // 超时检测：记录上次成功读取的时间
    auto last_successful_read_time = std::chrono::steady_clock::now();
    const auto capture_timeout = std::chrono::seconds(5);  // 5秒超时
    int consecutive_reads_without_frame = 0;
    const int max_consecutive_failures = 50;  // 50次连续读取失败后判定为超时

    while (!stop_thread_) {
        // 检查超时：距离上次成功读取是否超过阈值
        auto now = std::chrono::steady_clock::now();
        auto time_since_last_read = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_successful_read_time);

        if (time_since_last_read > capture_timeout) {
            LOG_ERROR("Camera capture timeout detected (no frame for " + std::to_string(time_since_last_read.count()) + "ms)");
            LOG_ERROR("Camera may be disconnected or malfunctioning, stopping capture thread");
            break;
        }

        // Read frame from camera
        if (!cap->read(frame)) {
            LOG_WARNING("Failed to read frame from camera, retrying...");
            retry_count++;
            consecutive_reads_without_frame++;

            // 检查连续失败次数是否过多
            if (consecutive_reads_without_frame > max_consecutive_failures) {
                LOG_ERROR("Camera read failed " + std::to_string(consecutive_reads_without_frame) + " consecutive times, stopping capture");
                break;
            }

            // Check if we should stop
            if (stop_thread_) {
                break;
            }

            // Limit retry attempts to avoid infinite loop
            if (retry_count > max_retries) {
                LOG_ERROR("Max retry attempts reached, stopping capture");
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Reset retry count on successful frame read
        retry_count = 0;
        consecutive_reads_without_frame = 0;
        last_successful_read_time = std::chrono::steady_clock::now();
        
        // Check if we should stop after successful read but before processing
        if (stop_thread_) {
            break;
        }
        
        // 记录帧信息（每300帧记录一次，约10秒@30fps）
        static int capture_count = 0;
        capture_count++;
        if (capture_count % 300 == 0) {
            LOG_INFO("Captured frame: " + std::to_string(frame.cols) + "x" + std::to_string(frame.rows) + ", count: " + std::to_string(capture_count));
        }
        
        // Resize if needed
        if (frame.size() != cv::Size(output_width_, output_height_)) {
            cv::resize(frame, frame, cv::Size(output_width_, output_height_));
        }
        
        // Convert to RGBA format
        cv::cvtColor(frame, rgba_frame, cv::COLOR_BGR2RGBA);
        
        // Create VideoFrame object with timestamp
        auto video_frame = std::make_shared<VideoFrame>(output_width_, output_height_);
        
        // Copy RGBA data to VideoFrame
        int frame_size = output_width_ * output_height_ * 4;
        std::memcpy(video_frame->data.get(), rgba_frame.data, frame_size);
        
        // Set timestamp (use current time in milliseconds)
        auto timestamp_now = std::chrono::system_clock::now();
        auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp_now.time_since_epoch());
        video_frame->timestamp_ms = timestamp_ms.count();
        
        // Update latest frame with thread safety
        {
            std::unique_lock<std::mutex> lock(frame_mutex_);
            latest_frame_ = std::move(video_frame);
        }
        
        // Notify waiting threads
        frame_cv_.notify_one();
        
        // Check if we should stop before sleeping
        if (stop_thread_) {
            break;
        }
        
        // Sleep to maintain desired frame rate
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / fps_));
    }
}

bool VideoEngine::initialize_opencv() {
    LOG_INFO("Entering initialize_opencv()");
    
    if (!opencv_initialized_) {
        LOG_ERROR("OpenCV not initialized");
        return false;
    }
    
    // 释放现有资源
    release_opencv();
    
    // 创建VideoCapture对象
    LOG_INFO("Creating VideoCapture object");
    cv::VideoCapture* cap = new cv::VideoCapture();
    
    // 使用DirectShow格式的设备名称打开摄像头
    // OpenCV在Windows上支持dshow格式: "设备名称"
    std::string device_name = selected_camera_;
    LOG_INFO("Selected camera: " + device_name);
    bool opened = false;
    
    // 尝试不同的打开方式
    try {
        // 方式1: 尝试直接使用设备名称打开
        LOG_INFO("Attempting to open camera directly with device name: " + device_name);
        opened = cap->open(device_name, cv::CAP_DSHOW);
        if (opened) {
            LOG_INFO("Successfully opened camera directly: " + device_name);
        } else {
            LOG_WARNING("Failed to open camera directly: " + device_name);
        }
        
        if (!opened) {
            // 方式2: 尝试使用dshow://前缀
            std::string dshow_path = "dshow://" + device_name;
            LOG_INFO("Attempting to open camera with dshow:// prefix: " + dshow_path);
            opened = cap->open(dshow_path, cv::CAP_DSHOW);
            if (opened) {
                LOG_INFO("Successfully opened camera with dshow:// prefix: " + dshow_path);
            } else {
                LOG_WARNING("Failed to open camera with dshow:// prefix: " + dshow_path);
            }
        }
        
        if (!opened) {
            // 方式3: 尝试将设备名称作为整数索引
            try {
                int camera_id = std::stoi(device_name);
                LOG_INFO("Attempting to open camera with ID: " + std::to_string(camera_id));
                opened = cap->open(camera_id, cv::CAP_DSHOW);
                if (opened) {
                    LOG_INFO("Successfully opened camera with ID: " + std::to_string(camera_id));
                } else {
                    LOG_WARNING("Failed to open camera with ID: " + std::to_string(camera_id));
                }
            } catch (const std::invalid_argument& e) {
                LOG_INFO("Device name is not an integer, skipping ID open attempt");
            }
        }
        
        if (!opened) {
            // 方式4: 尝试0-9的ID
            LOG_INFO("Attempting to open camera with IDs 0-9");
            for (int i = 0; i < 10 && !opened; ++i) {
                LOG_INFO("Attempting to open camera with ID: " + std::to_string(i));
                opened = cap->open(i, cv::CAP_DSHOW);
                if (opened) {
                    LOG_INFO("Successfully opened camera with ID: " + std::to_string(i));
                    break;
                } else {
                    LOG_WARNING("Failed to open camera with ID: " + std::to_string(i));
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Exception when initializing OpenCV: " + std::string(e.what()));
        delete cap;
        return false;
    }
    
    if (!opened) {
        LOG_ERROR("Failed to open camera using any method: " + device_name);
        delete cap;
        return false;
    }
    
    // 设置捕获属性
    LOG_INFO("Setting capture properties");
    cap->set(cv::CAP_PROP_FRAME_WIDTH, output_width_);
    cap->set(cv::CAP_PROP_FRAME_HEIGHT, output_height_);
    cap->set(cv::CAP_PROP_FPS, fps_);
    
    int actual_width = static_cast<int>(cap->get(cv::CAP_PROP_FRAME_WIDTH));
    int actual_height = static_cast<int>(cap->get(cv::CAP_PROP_FRAME_HEIGHT));
    int actual_fps = static_cast<int>(cap->get(cv::CAP_PROP_FPS));
    LOG_INFO("Actual camera properties: " + std::to_string(actual_width) + "x" + std::to_string(actual_height) + " at " + std::to_string(actual_fps) + "fps");
    
    // 设置OpenCV上下文指针
    cv_video_capture_ = cap;
    
    LOG_INFO("OpenCV initialized successfully for camera: " + device_name);
    return true;
}

void VideoEngine::release_opencv() {
    try {
        if (cv_video_capture_) {
            cv::VideoCapture* cap = static_cast<cv::VideoCapture*>(cv_video_capture_);
            if (cap && cap->isOpened()) {
                cap->release();
            }
            delete cap;
            cv_video_capture_ = nullptr;
        }
        opencv_initialized_ = false;
        LOG_INFO("OpenCV resources released successfully");
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during OpenCV resource release: " + std::string(e.what()));
        // Ensure we don't leave dangling pointers even on exception
        cv_video_capture_ = nullptr;
        opencv_initialized_ = false;
    }
}

VideoFrame::VideoFrame(int w, int h)
    : format(PixelFormat::RGBA),
      width(w),
      height(h),
      stride(w * 4),
      stride_uv(0),
      timestamp_ms(0) {
    data = std::make_unique<uint8_t[]>(stride * height);
    if (data) {
        memset(data.get(), 0, stride * height);
    }
}

} // namespace live_assistant