#include "scene_manager/printwindow_capture_source.h"
#include "common/log.h"
#include "common/media_clock.h"
#include <windows.h>
#include <wingdi.h>
#include <QImage>
#include <thread>
#include <chrono>

#pragma comment(lib, "gdi32.lib")

namespace live_assistant {

PrintWindowCaptureSource::PrintWindowCaptureSource(const CaptureConfig& config)
    : config_(config) {
}

PrintWindowCaptureSource::~PrintWindowCaptureSource() {
    shutdown();
}

bool PrintWindowCaptureSource::initialize() {
    // nothing heavy to init for PrintWindow path
    return true;
}

bool PrintWindowCaptureSource::start() {
    if (running_.exchange(true)) return true;
    // Launch worker
    worker_ = std::thread(&PrintWindowCaptureSource::worker_loop, this);
    return true;
}

bool PrintWindowCaptureSource::stop() {
    if (!running_.exchange(false)) return true;
    if (worker_.joinable()) worker_.join();
    return true;
}

bool PrintWindowCaptureSource::shutdown() {
    stop();
    std::lock_guard<std::mutex> lk(cb_mutex_);
    frame_cb_ = nullptr;
    return true;
}

void PrintWindowCaptureSource::set_frame_callback(CaptureFrameCallback cb) {
    std::lock_guard<std::mutex> lk(cb_mutex_);
    frame_cb_ = cb;
}

bool PrintWindowCaptureSource::is_running() const {
    return running_.load();
}

void PrintWindowCaptureSource::worker_loop() {
    HWND hwnd = nullptr;
    try {
        if (!config_.target_id.empty()) {
            hwnd = reinterpret_cast<HWND>(std::stoull(config_.target_id));
        }
    } catch (...) {
        hwnd = nullptr;
    }

    const int interval_ms = (config_.fps > 0) ? (1000 / config_.fps) : 33;

    while (running_) {
        if (!hwnd) {
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            continue;
        }

        // Capture via PrintWindow into DIB
        HDC hdcWindow = GetDC(hwnd);
        if (!hdcWindow) {
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            continue;
        }

        RECT rect;
        if (!GetWindowRect(hwnd, &rect)) {
            ReleaseDC(hwnd, hdcWindow);
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            continue;
        }
        int w = rect.right - rect.left;
        int h = rect.bottom - rect.top;
        if (w <= 0 || h <= 0) {
            ReleaseDC(hwnd, hdcWindow);
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            continue;
        }

        HDC hdcMem = CreateCompatibleDC(hdcWindow);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* pBits = nullptr;
        HBITMAP hDIB = CreateDIBSection(hdcWindow, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
        if (!hDIB) {
            DeleteDC(hdcMem);
            ReleaseDC(hwnd, hdcWindow);
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            continue;
        }
        HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hDIB);

        BOOL ok = PrintWindow(hwnd, hdcMem, 0);
        if (ok && pBits) {
            QImage img((uchar*)pBits, w, h, QImage::Format_ARGB32);
            CaptureFrame frame;
            frame.image = img.copy();
            frame.timestamp = MediaClock().now();
            frame.width = w;
            frame.height = h;

            CaptureFrameCallback cbcopy;
            {
                std::lock_guard<std::mutex> lk(cb_mutex_);
                cbcopy = frame_cb_;
            }
            if (cbcopy) cbcopy(frame);
        }

        SelectObject(hdcMem, hOld);
        DeleteObject(hDIB);
        DeleteDC(hdcMem);
        ReleaseDC(hwnd, hdcWindow);

        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
}

} // namespace live_assistant

