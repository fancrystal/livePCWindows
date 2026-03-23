#include "scene_manager/opencv_camera_capture_source.h"
#include "common/log.h"

#include <chrono>

#include <opencv2/opencv.hpp>

namespace live_assistant {

OpenCVCameraCaptureSource::OpenCVCameraCaptureSource(const CaptureConfig& config)
    : config_(config) {
}

OpenCVCameraCaptureSource::~OpenCVCameraCaptureSource() {
    stop();
    shutdown();
}

bool OpenCVCameraCaptureSource::initialize() {
    // 防止重复初始化
    if (initialized_.load()) {
        LOG_INFO("OpenCVCameraCaptureSource: already initialized, skipping");
        return true;
    }

    if (config_.type != CaptureConfig::TargetType::CAMERA) {
        LOG_ERROR("OpenCVCameraCaptureSource: invalid config type");
        return false;
    }

    int index = -1;
    cv::VideoCapture* cap = nullptr;
    bool opened = false;

    // Check if target_id is a simple number (index)
    bool is_likely_ffmpeg_name = config_.target_id.find("@device") != std::string::npos;

    if (is_likely_ffmpeg_name) {
        // FFmpeg device name format - skip trying to open it, go directly to fallback enumeration
        LOG_INFO("OpenCVCameraCaptureSource: detected FFmpeg device name format, using fallback enumeration");
    } else {
        // Try parsing as integer first
        try {
            index = std::stoi(config_.target_id);
            cap = new cv::VideoCapture();
            opened = cap->open(index, cv::CAP_DSHOW);
            if (!opened) {
                opened = cap->open(index);
            }
            // If still not opened, clean up the allocated VideoCapture
            if (!opened) {
                delete cap;
                cap = nullptr;
            }
        } catch (...) {
            // Not a number, clean up if allocated
            if (cap) {
                delete cap;
                cap = nullptr;
            }
        }
    }

    // If not opened yet, fallback to enumerating cameras
    if (!opened) {
        LOG_WARNING("OpenCVCameraCaptureSource: direct open failed, trying fallback enumeration");
        for (int i = 0; i < 10; ++i) {
            cap = new cv::VideoCapture();
            if (cap->open(i, cv::CAP_DSHOW)) {
                cv::Mat test_frame;
                if (cap->read(test_frame) && !test_frame.empty()) {
                    LOG_INFO("OpenCVCameraCaptureSource: opened camera at index " + std::to_string(i));
                    index = i;
                    opened = true;
                    break;
                }
                cap->release();
            }
            delete cap;
            cap = nullptr;
        }

        if (!opened) {
            LOG_ERROR("OpenCVCameraCaptureSource: all fallback methods failed");
            return false;
        }
    }

    if (config_.fps > 0) {
        cap->set(cv::CAP_PROP_FPS, static_cast<double>(config_.fps));
    }

    // 设置分辨率
    if (config_.width > 0 && config_.height > 0) {
        cap->set(cv::CAP_PROP_FRAME_WIDTH, static_cast<double>(config_.width));
        cap->set(cv::CAP_PROP_FRAME_HEIGHT, static_cast<double>(config_.height));
        LOG_INFO("OpenCVCameraCaptureSource: setting resolution to " +
                 std::to_string(config_.width) + "x" + std::to_string(config_.height));

        // 验证实际分辨率
        double actual_w = cap->get(cv::CAP_PROP_FRAME_WIDTH);
        double actual_h = cap->get(cv::CAP_PROP_FRAME_HEIGHT);
        LOG_INFO("OpenCVCameraCaptureSource: actual resolution is " +
                 std::to_string(static_cast<int>(actual_w)) + "x" +
                 std::to_string(static_cast<int>(actual_h)));
    }

    // OpenCV 不直接支持设置像素格式，但可以尝试设置 FOURCC
    // MJPG 格式可以获得更高的帧率
    if (config_.pixel_format == PixelFormat::MJPG) {
        cap->set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
        LOG_INFO("OpenCVCameraCaptureSource: trying to set MJPG format");
    }

    cap_ = cap;
    initialized_ = true;
    LOG_INFO("OpenCVCameraCaptureSource initialized: index=" + std::to_string(index));
    return true;
}

bool OpenCVCameraCaptureSource::start() {
    if (running_.load()) return true;
    if (!cap_) {
        LOG_ERROR("OpenCVCameraCaptureSource: not initialized");
        return false;
    }

    stop_flag_ = false;
    running_ = true;

    th_ = std::thread(&OpenCVCameraCaptureSource::capture_loop, this);
    return true;
}

bool OpenCVCameraCaptureSource::stop() {
    if (!running_.load()) {
        LOG_INFO("OpenCVCameraCaptureSource::stop() - not running, skipping");
        return true;
    }

    LOG_INFO("OpenCVCameraCaptureSource::stop() - stopping capture thread");
    stop_flag_ = true;
    if (th_.joinable()) {
        th_.join();
    }

    running_ = false;
    LOG_INFO("OpenCVCameraCaptureSource::stop() - capture thread stopped");
    return true;
}

bool OpenCVCameraCaptureSource::shutdown() {
    LOG_INFO("OpenCVCameraCaptureSource::shutdown() - cap_=" + std::string(cap_ ? "not null" : "null"));
    if (cap_) {
        auto* cap = static_cast<cv::VideoCapture*>(cap_);
        bool was_opened = cap->isOpened();
        LOG_INFO("OpenCVCameraCaptureSource::shutdown() - was_opened=" + std::string(was_opened ? "true" : "false"));
        if (was_opened) {
            cap->release();
            LOG_INFO("OpenCVCameraCaptureSource::shutdown() - released VideoCapture");
        }
        delete cap;
        cap_ = nullptr;
        LOG_INFO("OpenCVCameraCaptureSource::shutdown() - deleted VideoCapture object");
    }
    initialized_ = false;
    LOG_INFO("OpenCVCameraCaptureSource::shutdown() - complete, initialized_=false");
    return true;
}

void OpenCVCameraCaptureSource::capture_loop() {
    auto* cap = static_cast<cv::VideoCapture*>(cap_);
    if (!cap) {
        running_ = false;
        return;
    }

    cv::Mat frame_bgr;
    cv::Mat frame_bgra;

    const int target_fps = config_.fps > 0 ? config_.fps : 30;
    const auto frame_interval = std::chrono::milliseconds(1000 / target_fps);

    while (!stop_flag_.load()) {
        if (!cap->read(frame_bgr) || frame_bgr.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // BGR -> BGRA (Qt expects BGRA for Format_ARGB32)
        cv::cvtColor(frame_bgr, frame_bgra, cv::COLOR_BGR2BGRA);

        QImage img(frame_bgra.data, frame_bgra.cols, frame_bgra.rows, static_cast<int>(frame_bgra.step), QImage::Format_ARGB32);
        QImage deep = img.copy();

        CaptureFrame out;
        out.image = std::move(deep);
        // Use wallclock timestamp as media timestamp (microseconds).
        out.timestamp.us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        out.width = out.image.width();
        out.height = out.image.height();

        emit frameReady(out);

        std::this_thread::sleep_for(frame_interval);
    }
}

} // namespace live_assistant
