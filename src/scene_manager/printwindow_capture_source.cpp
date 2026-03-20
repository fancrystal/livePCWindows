#include "scene_manager/printwindow_capture_source.h"
#include "common/log.h"
#include "common/media_clock.h"
#include <windows.h>
#include <wingdi.h>
#include <QImage>
#include <thread>
#include <chrono>
#include <cstring>

#pragma comment(lib, "gdi32.lib")

namespace live_assistant {

PrintWindowCaptureSource::PrintWindowCaptureSource(const CaptureConfig& config)
    : config_(config) {
}

PrintWindowCaptureSource::~PrintWindowCaptureSource() {
    shutdown();
}

bool PrintWindowCaptureSource::initialize() {
    return true;
}

bool PrintWindowCaptureSource::start() {
    if (running_.exchange(true)) return true;
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
    cleanup_dib();
    return true;
}

bool PrintWindowCaptureSource::is_running() const {
    return running_.load();
}

void PrintWindowCaptureSource::cleanup_dib() {
    if (cached_hdc_mem_ && cached_old_bitmap_) {
        SelectObject(cached_hdc_mem_, cached_old_bitmap_);
        cached_old_bitmap_ = nullptr;
    }
    if (cached_hbitmap_) {
        DeleteObject(cached_hbitmap_);
        cached_hbitmap_ = nullptr;
    }
    if (cached_hdc_mem_) {
        DeleteDC(cached_hdc_mem_);
        cached_hdc_mem_ = nullptr;
    }
    cached_bits_ = nullptr;
    cached_width_ = 0;
    cached_height_ = 0;
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

        // Reuse cached DIB if size matches, otherwise recreate
        if (!cached_hbitmap_ || cached_width_ != w || cached_height_ != h) {
            cleanup_dib();

            cached_hdc_mem_ = CreateCompatibleDC(hdcWindow);
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = w;
            bmi.bmiHeader.biHeight = -h;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            cached_hbitmap_ = CreateDIBSection(hdcWindow, &bmi, DIB_RGB_COLORS, &cached_bits_, NULL, 0);
            if (!cached_hbitmap_) {
                DeleteDC(cached_hdc_mem_);
                cached_hdc_mem_ = nullptr;
                ReleaseDC(hwnd, hdcWindow);
                std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
                continue;
            }
            cached_old_bitmap_ = (HBITMAP)SelectObject(cached_hdc_mem_, cached_hbitmap_);
            cached_width_ = w;
            cached_height_ = h;
        }

        BOOL ok = PrintWindow(hwnd, cached_hdc_mem_, 0);
        if (ok && cached_bits_) {
            // PrintWindow returns BGRA data
            // Use Format_ARGB32 which matches BGRA on little-endian systems
            QImage img((uchar*)cached_bits_, w, h, QImage::Format_ARGB32);

            CaptureFrame frame;
            frame.image = img.copy();
            frame.timestamp = MediaClock().now();
            frame.width = w;
            frame.height = h;

            emit frameReady(frame);
        }

        ReleaseDC(hwnd, hdcWindow);

        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }

    // Final cleanup
    cleanup_dib();
}

} // namespace live_assistant