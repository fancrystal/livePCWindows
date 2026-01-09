#include "app/screen_capture_selector.h"
#include "common/log.h"
#include <QApplication>
#include <QScreen>
#include <QListWidgetItem>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QPixmap>
#include <QPainter>
#include <QIcon>
#include <QWindow>
#include <algorithm>
#include <thread>
#include <QMutexLocker>

// Windows API headers for DWM
#include <dwmapi.h>
#include <windows.h>
#include <wingdi.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

#undef min
#undef max

namespace live_assistant {

// Helper: capture window to QImage using PrintWindow+DIB (runs in worker thread)
static QImage capture_window_dib_image(HWND hwnd, int capW, int capH) {
    HDC hdcWindow = GetDC(hwnd);
    if (!hdcWindow) {
        return QImage();
    }
    HDC hdcMem = CreateCompatibleDC(hdcWindow);
    if (!hdcMem) {
        ReleaseDC(hwnd, hdcWindow);
        return QImage();
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = capW;
    bmi.bmiHeader.biHeight = -capH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hDIB = CreateDIBSection(hdcWindow, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    if (!hDIB) {
        DeleteDC(hdcMem);
        ReleaseDC(hwnd, hdcWindow);
        return QImage();
    }

    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hDIB);

    BOOL success = PrintWindow(hwnd, hdcMem, 0);
    QImage result;
    if (success && pBits) {
        QImage image((uchar*)pBits, capW, capH, QImage::Format_ARGB32);
        result = image.copy(); // deep copy to own memory
    }

    SelectObject(hdcMem, hOld);
    DeleteObject(hDIB);
    DeleteDC(hdcMem);
    ReleaseDC(hwnd, hdcWindow);
    return result;
}

ScreenCaptureSelector::ScreenCaptureSelector(QWidget *parent)
    : QDialog(parent)
    , selected_target_(nullptr) {
    setup_ui();

    // Set up timer for updating thumbnails
    update_timer_ = new QTimer(this);
    connect(update_timer_, &QTimer::timeout, this, &ScreenCaptureSelector::update_thumbnails);
    update_timer_->start(1000); // Update every second

    // Initially refresh targets
    refresh_targets();

    LOG_INFO("ScreenCaptureSelector created");
}

ScreenCaptureSelector::~ScreenCaptureSelector() {
    update_timer_->stop();
    LOG_INFO("ScreenCaptureSelector destroyed");
}

void ScreenCaptureSelector::setup_ui() {
    setWindowTitle("选择捕获目标");
    setMinimumSize(800, 600);
    setStyleSheet(
        "QDialog { background-color: #1a1a1a; color: #ffffff; }"
        "QWidget { background-color: #1a1a1a; color: #ffffff; }"
        "QListWidget { background-color: #2a2a2a; color: #ffffff; border: 1px solid #444444; }"
        "QListWidget::item { padding: 5px; border-bottom: 1px solid #333333; }"
        "QListWidget::item:selected { background-color: #444444; }"
        "QLabel { color: #ffffff; }"
        "QPushButton { background-color: #333333; color: #ffffff; border: 1px solid #444444; border-radius: 4px; padding: 8px 16px; }"
        "QPushButton:hover { background-color: #444444; }"
        "QPushButton:pressed { background-color: #555555; }"
        "QPushButton:disabled { background-color: #222222; color: #666666; }"
        "QGroupBox { color: #ffffff; border: 1px solid #333333; border-radius: 4px; margin-top: 10px; }"
        "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; padding: 0 5px; }"
    );

    auto* main_layout = new QHBoxLayout(this);

    // Left panel - target list
    auto* left_panel = new QWidget();
    left_panel->setMinimumWidth(300);
    left_panel->setMaximumWidth(400);
    auto* left_layout = new QVBoxLayout(left_panel);

    // Header with refresh button
    auto* header_layout = new QHBoxLayout();
    auto* header_label = new QLabel("可捕获的目标:");
    header_label->setStyleSheet("font-weight: bold; font-size: 14px;");
    refresh_button_ = new QPushButton("刷新");
    refresh_button_->setMaximumWidth(80);
    connect(refresh_button_, &QPushButton::clicked, this, &ScreenCaptureSelector::refresh_targets);

    header_layout->addWidget(header_label);
    header_layout->addStretch();
    header_layout->addWidget(refresh_button_);
    left_layout->addLayout(header_layout);

    // List widget
    list_widget_ = new QListWidget();
    list_widget_->setIconSize(QSize(120, 80));
    connect(list_widget_, &QListWidget::itemSelectionChanged, this, &ScreenCaptureSelector::on_item_selection_changed);
    left_layout->addWidget(list_widget_);

    main_layout->addWidget(left_panel);

    // Right panel - preview
    auto* right_panel = new QWidget();
    right_panel->setMinimumWidth(400);
    auto* right_layout = new QVBoxLayout(right_panel);

    auto* preview_group = new QGroupBox("预览");
    auto* preview_layout = new QVBoxLayout(preview_group);

    preview_label_ = new QLabel("请选择一个捕获目标");
    preview_label_->setMinimumSize(320, 240);
    preview_label_->setAlignment(Qt::AlignCenter);
    preview_label_->setStyleSheet("border: 2px dashed #444444; background-color: #2a2a2a;");
    preview_layout->addWidget(preview_label_);

    right_layout->addWidget(preview_group);
    right_layout->addStretch();

    // Buttons
    auto* button_layout = new QHBoxLayout();
    button_layout->addStretch();

    ok_button_ = new QPushButton("确定");
    ok_button_->setEnabled(false);
    connect(ok_button_, &QPushButton::clicked, this, &ScreenCaptureSelector::on_ok_clicked);

    cancel_button_ = new QPushButton("取消");
    connect(cancel_button_, &QPushButton::clicked, this, &ScreenCaptureSelector::on_cancel_clicked);

    button_layout->addWidget(ok_button_);
    button_layout->addWidget(cancel_button_);

    right_layout->addLayout(button_layout);

    main_layout->addWidget(right_panel);
}

void ScreenCaptureSelector::refresh_targets() {
    LOG_INFO("Refreshing capture targets");

    targets_.clear();
    list_widget_->clear();
    selected_target_ = nullptr;
    ok_button_->setEnabled(false);
    preview_label_->setText("请选择一个捕获目标");

    enumerate_screens();
    enumerate_windows();

    // Populate list widget (store index in UserRole to avoid pointer instability)
    for (int i = 0; i < static_cast<int>(targets_.size()); ++i) {
        const auto& target = targets_[i];
        auto* item = new QListWidgetItem();

        // Create display text
        QString display_text;
        if (target.type == CaptureTarget::Type::SCREEN) {
            display_text = QString("屏幕: %1 (%2x%3)")
                .arg(QString::fromStdString(target.name))
                .arg(target.size.width())
                .arg(target.size.height());
        } else {
            display_text = QString("窗口: %1 (%2x%3)")
                .arg(QString::fromStdString(target.name))
                .arg(target.size.width())
                .arg(target.size.height());
        }

        item->setText(display_text);
        item->setIcon(QIcon(target.thumbnail));
        item->setData(Qt::UserRole, i);

        list_widget_->addItem(item);
    }

    LOG_INFO("Found " + std::to_string(targets_.size()) + " capture targets");

    // Schedule async thumbnail updates for non-selected items (throttled)
    for (int i = 0; i < static_cast<int>(targets_.size()); ++i) {
        // small delay to avoid bursting work on refresh
        start_async_thumbnail_update(i);
    }
}

void ScreenCaptureSelector::enumerate_screens() {
    LOG_INFO("Enumerating screens");

    // Get all screens
    const auto screens = QApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        QScreen* screen = screens[i];
        CaptureTarget target;
        target.type = CaptureTarget::Type::SCREEN;
        target.id = screen->name().toStdString(); // Monitor device name
        target.name = screen->name().toStdString();
        target.size = screen->size();

        // Create thumbnail for screen (assign into target)
        create_thumbnail_for_screen(target);

        targets_.push_back(std::move(target));
        LOG_INFO("Found screen: " + targets_.back().name + " (" + std::to_string(targets_.back().size.width()) + "x" + std::to_string(targets_.back().size.height()) + ")");
    }
}

void ScreenCaptureSelector::enumerate_windows() {
    LOG_INFO("Enumerating windows");

    // Enumerate top-level windows
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* selector = reinterpret_cast<ScreenCaptureSelector*>(lParam);

        // Skip invisible windows
        if (!IsWindowVisible(hwnd)) {
            return TRUE;
        }

        // Skip windows without title
        wchar_t titleW[512];
        if (GetWindowTextW(hwnd, titleW, ARRAYSIZE(titleW)) == 0) {
            return TRUE;
        }

        // Skip system windows and our own window
        if (hwnd == reinterpret_cast<HWND>(selector->winId())) {
            return TRUE;
        }

        // Get window rect
        RECT rect;
        if (!GetWindowRect(hwnd, &rect)) {
            return TRUE;
        }

        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;

        // Skip very small windows
        if (width < 100 || height < 100) {
            return TRUE;
        }

        CaptureTarget target;
        target.type = CaptureTarget::Type::WINDOW;
        target.id = std::to_string(reinterpret_cast<uintptr_t>(hwnd)); // Use hwnd as ID
        target.name = QString::fromWCharArray(titleW).toStdString();
        target.size = QSize(width, height);

        // Create thumbnail for window (assign into target)
        selector->create_thumbnail_for_window(target);

        selector->targets_.push_back(std::move(target));
        LOG_INFO("Found window: " + selector->targets_.back().name + " (" + std::to_string(selector->targets_.back().size.width()) + "x" + std::to_string(selector->targets_.back().size.height()) + ")");

        return TRUE;
    }, reinterpret_cast<LPARAM>(this));
}

void ScreenCaptureSelector::create_thumbnail_for_screen(CaptureTarget& target) {
    // For screen, we'll create a simple colored rectangle with screen info
    QPixmap thumbnail(120, 80);
    thumbnail.fill(QColor(64, 64, 128)); // Dark blue for screens

    QPainter painter(&thumbnail);
    painter.setPen(Qt::white);
    painter.setFont(QFont("Arial", 8));
    painter.drawText(thumbnail.rect(), Qt::AlignCenter, QString("屏幕\n%1x%2").arg(target.size.width()).arg(target.size.height()));

    // Assign thumbnail into provided target
    target.thumbnail = thumbnail;
}

void ScreenCaptureSelector::create_thumbnail_for_window(CaptureTarget& target) {
    // Try to create a real thumbnail using PrintWindow or DWM
    HWND hwnd = reinterpret_cast<HWND>(std::stoull(target.id));
    QPixmap thumbnail = create_window_thumbnail(hwnd, 120, 80);

    // If PrintWindow failed, create a placeholder
    if (thumbnail.isNull()) {
        thumbnail = QPixmap(120, 80);
        thumbnail.fill(QColor(128, 64, 64)); // Dark red for windows

        QPainter painter(&thumbnail);
        painter.setPen(Qt::white);
        painter.setFont(QFont("Arial", 7));
        painter.drawText(thumbnail.rect(), Qt::AlignCenter, QString("窗口\n%1").arg(QString::fromStdString(target.name).left(10)));
    }

    // Assign thumbnail into provided target
    target.thumbnail = thumbnail;
}

QPixmap ScreenCaptureSelector::create_window_thumbnail(HWND hwnd, int width, int height) {
    // Try to use DWM thumbnail first
    HTHUMBNAIL thumbnail;
    HRESULT hr = DwmRegisterThumbnail(hwnd, (HWND)this->winId(), &thumbnail);
    if (SUCCEEDED(hr)) {
        // Get thumbnail properties
        DWM_THUMBNAIL_PROPERTIES props = {};
        SIZE sourceSize = {};
        hr = DwmQueryThumbnailSourceSize(thumbnail, &sourceSize);
        if (SUCCEEDED(hr)) {
            props.rcSource = {0, 0, sourceSize.cx, sourceSize.cy};
        }
        if (SUCCEEDED(hr)) {
            // Set thumbnail properties
            props.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE;
            props.fSourceClientAreaOnly = FALSE;
            props.fVisible = TRUE;
            props.opacity = 255;
            props.rcSource = {0, 0, props.rcSource.right, props.rcSource.bottom};
            props.rcDestination = {0, 0, width, height};

            hr = DwmUpdateThumbnailProperties(thumbnail, &props);
            if (SUCCEEDED(hr)) {
                // Create a temporary native widget to host the thumbnail (offscreen)
                QWidget* temp = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint);
                temp->setAttribute(Qt::WA_NativeWindow);
                temp->resize(width, height);
                // Move off-screen to avoid flicker
                temp->move(-10000, -10000);
                temp->show();

                // Give DWM a moment to render into the temp window
                QCoreApplication::processEvents();
                Sleep(60);

                // Grab the temp window contents
                QPixmap pixmap = QGuiApplication::primaryScreen()->grabWindow((WId)temp->winId(), 0, 0, width, height);

                temp->hide();
                delete temp;

                DwmUnregisterThumbnail(thumbnail);

                if (!pixmap.isNull()) {
                    return pixmap;
                }
            } else {
                DwmUnregisterThumbnail(thumbnail);
            }
        } else {
            DwmUnregisterThumbnail(thumbnail);
        }
    }

    // Fall back to PrintWindow with improved capture into DIBSection (avoids extra BitBlt copy)
    HDC hdcWindow = GetDC(hwnd);
    if (!hdcWindow) {
        return QPixmap();
    }

    HDC hdcMem = CreateCompatibleDC(hdcWindow);
    if (!hdcMem) {
        ReleaseDC(hwnd, hdcWindow);
        return QPixmap();
    }

    RECT rect;
    GetWindowRect(hwnd, &rect);
    int windowWidth = rect.right - rect.left;
    int windowHeight = rect.bottom - rect.top;

    // Determine DPI for this window (use GetDpiForWindow if available) and scale capture limits
    UINT dpi = 96;
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        typedef UINT (WINAPI *GetDpiForWindow_t)(HWND);
        GetDpiForWindow_t pGetDpiForWindow = (GetDpiForWindow_t)GetProcAddress(user32, "GetDpiForWindow");
        if (pGetDpiForWindow) {
            dpi = pGetDpiForWindow(hwnd);
        }
    }

    // Limit capture size to avoid huge allocations; scale limit by DPI to keep visual fidelity
    const int baseMaxCapture = 1600;
    double dpiScale = static_cast<double>(dpi) / 96.0;
    int maxCaptureDim = static_cast<int>(std::max<double>(640.0, baseMaxCapture * dpiScale));
    int capW = (windowWidth < maxCaptureDim) ? windowWidth : maxCaptureDim;
    int capH = (windowHeight < maxCaptureDim) ? windowHeight : maxCaptureDim;

    // Prepare DIB section (top-down BGRA)
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = capW;
    bmi.bmiHeader.biHeight = -capH; // negative => top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hDIB = CreateDIBSection(hdcWindow, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    if (!hDIB) {
        DeleteDC(hdcMem);
        ReleaseDC(hwnd, hdcWindow);
        return QPixmap();
    }

    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hDIB);

    // Try PrintWindow with PW_RENDERFULLCONTENT if available (better for some apps)
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif
    BOOL success = FALSE;
    // Try rendering full content first
    success = PrintWindow(hwnd, hdcMem, PW_RENDERFULLCONTENT);
    if (!success) {
        // Fallback to default PrintWindow behavior
        success = PrintWindow(hwnd, hdcMem, 0);
    }

    QPixmap result;
    if (success) {
        // Create QImage from DIB bits (BGRA)
        QImage image((uchar*)pBits, capW, capH, QImage::Format_ARGB32);
        // Scale to requested thumbnail size with smooth transformation
        result = QPixmap::fromImage(image).scaled(width, height, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    // Cleanup
    SelectObject(hdcMem, hOld);
    DeleteObject(hDIB);
    DeleteDC(hdcMem);
    ReleaseDC(hwnd, hdcWindow);

    if (!result.isNull()) {
        return result;
    }

    // If still empty, create a placeholder showing that preview is unavailable
    wchar_t titleW[512] = {};
    GetWindowTextW(hwnd, titleW, ARRAYSIZE(titleW));
    QString title = QString::fromWCharArray(titleW).trimmed();
    if (title.isEmpty()) {
        title = QStringLiteral("无法预览");
    } else {
        title = QStringLiteral("无法预览: %1").arg(title);
    }

    QPixmap placeholder(width, height);
    placeholder.fill(QColor(48, 48, 48));
    QPainter painter(&placeholder);
    painter.setPen(Qt::white);
    painter.setFont(QFont("Arial", 10));
    painter.drawText(placeholder.rect(), Qt::AlignCenter | Qt::TextWordWrap, title);
    painter.end();

    return placeholder;
}

void ScreenCaptureSelector::on_item_selection_changed() {
    auto* current_item = list_widget_->currentItem();
    if (!current_item) {
        selected_target_ = nullptr;
        ok_button_->setEnabled(false);
        preview_label_->setText("请选择一个捕获目标");
        return;
    }

    int idx = current_item->data(Qt::UserRole).toInt();
    if (idx >= 0 && idx < static_cast<int>(targets_.size())) {
        selected_target_ = &targets_[idx];
    } else {
        selected_target_ = nullptr;
    }
    ok_button_->setEnabled(true);

    if (selected_target_) {
        // Try DWM thumbnail rendering into the preview label for higher-quality preview (windows only)
        bool shown = false;
        if (selected_target_->type == CaptureTarget::Type::WINDOW) {
            HWND srcHwnd = reinterpret_cast<HWND>(std::stoull(selected_target_->id));
            HTHUMBNAIL thumb = nullptr;
            HRESULT hr = DwmRegisterThumbnail(srcHwnd, (HWND)preview_label_->winId(), &thumb);
            if (SUCCEEDED(hr) && thumb) {
                // Set thumbnail properties to render into preview_label_ area
                DWM_THUMBNAIL_PROPERTIES props = {};
                props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY;
                props.fVisible = TRUE;
                props.opacity = 255;
                int pw = preview_label_->width();
                int ph = preview_label_->height();
                props.rcDestination = {0, 0, pw, ph};
                hr = DwmUpdateThumbnailProperties(thumb, &props);
                if (SUCCEEDED(hr)) {
                    // Let Qt event loop update the thumbnail rendering
                    QCoreApplication::processEvents();
                    Sleep(60); // small delay for DWM to render
                    QPixmap grab = QGuiApplication::primaryScreen()->grabWindow((WId)preview_label_->winId(), 0, 0, pw, ph);
                    if (!grab.isNull()) {
                        preview_label_->setPixmap(grab);
                        shown = true;
                    }
                }
                DwmUnregisterThumbnail(thumb);
            }
        }

        if (!shown) {
            // Fallback: use existing thumbnail and scale
            QPixmap scaled_preview = selected_target_->thumbnail.scaled(
                320, 240, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            preview_label_->setPixmap(scaled_preview);
        }

        QString preview_text = QString("预览 - %1\n尺寸: %2x%3")
            .arg(selected_target_->type == CaptureTarget::Type::SCREEN ? "屏幕" : "窗口")
            .arg(selected_target_->size.width())
            .arg(selected_target_->size.height());
        preview_label_->setText(""); // Clear text when showing pixmap
        preview_label_->setToolTip(preview_text);
    }
}

void ScreenCaptureSelector::update_thumbnails() {
    // Periodically update thumbnails for selected item
    if (selected_target_ && selected_target_->type == CaptureTarget::Type::WINDOW) {
        // Recreate thumbnail for selected window (guard parsing of id)
        HWND selHwnd = nullptr;
        try {
            if (!selected_target_->id.empty()) {
                selHwnd = reinterpret_cast<HWND>(std::stoull(selected_target_->id));
            }
        } catch (const std::exception& ex) {
            LOG_WARNING(std::string("Invalid window id for thumbnail: ") + selected_target_->id + " - " + ex.what());
            selHwnd = nullptr;
        }
        QPixmap new_thumbnail;
        if (selHwnd) {
            new_thumbnail = create_window_thumbnail(selHwnd, 120, 80);
        } else {
            new_thumbnail = QPixmap(120, 80);
            new_thumbnail.fill(QColor(80, 80, 80));
        }

        if (!new_thumbnail.isNull()) {
            // Update thumbnail in targets list
            // Update thumbnail in targets list and list widget item
            for (int i = 0; i < static_cast<int>(targets_.size()); ++i) {
                auto& target = targets_[i];
                if (target.id == selected_target_->id && target.type == selected_target_->type) {
                    target.thumbnail = new_thumbnail;
                    auto* item = list_widget_->item(i);
                    if (item) {
                        item->setIcon(QIcon(new_thumbnail));
                    }
                    break;
                }
            }

            // Update preview if it's the selected item
            QPixmap scaled_preview = new_thumbnail.scaled(
                320, 240, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            preview_label_->setPixmap(scaled_preview);
        }
    }
}

// Start an async thumbnail update for index if not updating and cooldown passed
void ScreenCaptureSelector::start_async_thumbnail_update(int index) {
    if (index < 0 || index >= static_cast<int>(targets_.size())) return;

    auto& target = targets_[index];
    using namespace std::chrono;
    auto now = steady_clock::now();
    const auto cooldown = seconds(1); // 1 second cooldown per target
    if (target.updating) return;
    if (now - target.last_update < cooldown) return;

    target.updating = true;
    // Launch a detached thread to capture into QImage, then post the result back to UI thread
    HWND hwnd = nullptr;
    try {
        if (!target.id.empty()) {
            hwnd = reinterpret_cast<HWND>(std::stoull(target.id));
        }
    } catch (const std::exception& ex) {
        LOG_WARNING(std::string("start_async_thumbnail_update: invalid target id: ") + target.id + " - " + ex.what());
        // mark not updating and return early
        target.updating = false;
        return;
    }
    int capW = std::min(target.size.width(), 1600);
    int capH = std::min(target.size.height(), 1600);

    // Capture in background
    std::thread([this, index, hwnd, capW, capH]() {
        QImage img = capture_window_dib_image(hwnd, capW, capH);
        // Post result back to UI thread
        QMetaObject::invokeMethod(this, [this, index, img]() {
            // convert to pixmap and update UI
            QPixmap pix;
            if (!img.isNull()) {
                pix = QPixmap::fromImage(img).scaled(120, 80, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            } else {
                pix = QPixmap(120, 80);
                pix.fill(QColor(80, 80, 80));
            }

            if (index >= 0 && index < static_cast<int>(targets_.size())) {
                targets_[index].thumbnail = pix;
                targets_[index].last_update = std::chrono::steady_clock::now();
                targets_[index].updating = false;
                auto* item = list_widget_->item(index);
                if (item) {
                    item->setIcon(QIcon(pix));
                }
                if (selected_target_ && selected_target_ == &targets_[index]) {
                    preview_label_->setPixmap(pix.scaled(320, 240, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                }
            }
        }, Qt::QueuedConnection);
    }).detach();
}

// Note: async thumbnail completion is handled inline via QMetaObject::invokeMethod in the worker thread.

void ScreenCaptureSelector::on_ok_clicked() {
    if (selected_target_) {
        LOG_INFO("Selected capture target: " + selected_target_->name);
        accept();
    }
}

void ScreenCaptureSelector::on_cancel_clicked() {
    selected_target_ = nullptr;
    LOG_INFO("Capture target selection cancelled");
    reject();
}

const CaptureTarget* ScreenCaptureSelector::get_selected_target() const {
    return selected_target_;
}

} // namespace live_assistant