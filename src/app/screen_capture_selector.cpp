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

    // Thumbnails are one-shot for now (no periodic refresh)
    update_timer_ = nullptr;

    // Initially refresh targets
    refresh_targets();

    LOG_INFO("ScreenCaptureSelector created");
}

ScreenCaptureSelector::~ScreenCaptureSelector() {
    if (update_timer_) {
        update_timer_->stop();
    }
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

        // Diagnostic log
        if (item->icon().isNull()) {
            LOG_WARNING("Item icon is null for target: " + target.name);
            if (target.thumbnail.isNull()) {
                LOG_WARNING(" -> Reason: The source QPixmap thumbnail was null.");
            }
        }
    }

    LOG_INFO("Found " + std::to_string(targets_.size()) + " capture targets");

    // Force an immediate repaint of the list widget
    list_widget_->update();
    QCoreApplication::processEvents();

    // One-shot thumbnails: no async refresh is needed.
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

    struct Candidate {
        HWND hwnd = nullptr;
        std::string title;
        QSize size;
    };

    std::vector<Candidate> candidates;

    // Phase 1: EnumWindows and filter by window attributes only (no thumbnail capture)
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* ctx = reinterpret_cast<std::vector<Candidate>*>(lParam);

        if (!IsWindowVisible(hwnd)) return TRUE;
        if (IsIconic(hwnd)) return TRUE;

        BOOL cloaked = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
            return TRUE;
        }

        LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

        if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

        wchar_t titleW[512];
        if (GetWindowTextW(hwnd, titleW, ARRAYSIZE(titleW)) == 0) return TRUE;

        RECT rect;
        if (!GetWindowRect(hwnd, &rect)) return TRUE;

        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;
        if (width < 100 || height < 100) return TRUE;

        Candidate c;
        c.hwnd = hwnd;
        c.title = QString::fromWCharArray(titleW).toStdString();
        c.size = QSize(width, height);
        ctx->push_back(std::move(c));

        return TRUE;
    }, reinterpret_cast<LPARAM>(&candidates));

    // Phase 2: capture thumbnails only for filtered candidates
    auto is_preview_unavailable = [](const QPixmap& pm) -> bool {
        if (pm.isNull()) return true;
        QImage img = pm.toImage();
        if (img.isNull() || img.width() == 0 || img.height() == 0) return true;
        const int w = img.width();
        const int h = img.height();
        const int stepX = std::max(1, w / 10);
        const int stepY = std::max(1, h / 10);
        int samples = 0;
        int darkCount = 0;
        for (int y = stepY / 2; y < h; y += stepY) {
            for (int x = stepX / 2; x < w; x += stepX) {
                QColor c(img.pixel(x, y));
                int lum = (c.red() * 299 + c.green() * 587 + c.blue() * 114) / 1000;
                if (lum < 16) darkCount++;
                samples++;
            }
        }
        if (samples == 0) return true;
        return (darkCount * 100 / samples) >= 95;
    };

    for (const auto& c : candidates) {
        // Skip our own window
        if (c.hwnd == reinterpret_cast<HWND>(this->winId())) {
            continue;
        }

        CaptureTarget target;
        target.type = CaptureTarget::Type::WINDOW;
        target.id = std::to_string(reinterpret_cast<uintptr_t>(c.hwnd));
        target.name = c.title;
        target.size = c.size;

        create_thumbnail_for_window(target);
        if (is_preview_unavailable(target.thumbnail)) {
            continue;
        }

        targets_.push_back(std::move(target));
        LOG_INFO("Found window: " + targets_.back().name + " (" + std::to_string(targets_.back().size.width()) + "x" + std::to_string(targets_.back().size.height()) + ")");
    }
}

QPixmap ScreenCaptureSelector::create_screen_thumbnail(const QString& device_name, int thumb_width, int thumb_height) {
    // Prefer Qt native grab (more robust for DPI/multi-monitor)
    const auto screens = QApplication::screens();
    QScreen* target_screen = nullptr;

    for (QScreen* screen : screens) {
        if (screen && screen->name() == device_name) {
            target_screen = screen;
            break;
        }
    }

    if (!target_screen) {
        LOG_WARNING("create_screen_thumbnail: screen not found for device_name=" + device_name.toStdString() + ", fallback to primary");
        target_screen = QApplication::primaryScreen();
    }

    if (!target_screen) {
        LOG_ERROR("create_screen_thumbnail: primaryScreen is null");
        QPixmap placeholder(thumb_width, thumb_height);
        placeholder.fill(QColor(64, 64, 128));
        return placeholder;
    }

    // Grab the screen content
    QPixmap grabbed = target_screen->grabWindow(0);
    if (grabbed.isNull()) {
        LOG_WARNING("create_screen_thumbnail: grabWindow returned null for device_name=" + device_name.toStdString());
        QPixmap placeholder(thumb_width, thumb_height);
        placeholder.fill(QColor(64, 64, 128));
        return placeholder;
    }

    // Scale to thumbnail size
    QPixmap thumb = grabbed.scaled(thumb_width, thumb_height, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (thumb.isNull()) {
        LOG_WARNING("create_screen_thumbnail: scaled thumbnail is null for device_name=" + device_name.toStdString());
    }
    return thumb;
}

void ScreenCaptureSelector::create_thumbnail_for_screen(CaptureTarget& target) {
    // Create actual screen thumbnail
    QPixmap thumbnail = create_screen_thumbnail(QString::fromStdString(target.id), list_widget_->iconSize().width(), list_widget_->iconSize().height());
    
    // If thumbnail is null or invalid, create a placeholder
    if (thumbnail.isNull()) {
        thumbnail = QPixmap(120, 80);
        thumbnail.fill(QColor(64, 64, 128)); // Dark blue for screens
        
        QPainter painter(&thumbnail);
        painter.setPen(Qt::white);
        painter.setFont(QFont("Arial", 8));
        painter.drawText(thumbnail.rect(), Qt::AlignCenter, 
                        QString("屏幕\n%1x%2").arg(target.size.width()).arg(target.size.height()));
    }
    
    // Assign thumbnail into provided target
    target.thumbnail = thumbnail;
}

void ScreenCaptureSelector::create_thumbnail_for_window(CaptureTarget& target) {
    // Try to create a real thumbnail using PrintWindow or DWM
    HWND hwnd = reinterpret_cast<HWND>(std::stoull(target.id));
    QPixmap thumbnail = create_window_thumbnail(hwnd, list_widget_->iconSize().width(), list_widget_->iconSize().height());

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
    // Skip invalid HWND
    if (!hwnd || !IsWindow(hwnd)) {
        LOG_WARNING("create_window_thumbnail: invalid HWND");
        return QPixmap();
    }

    // If window is not visible, best effort: still try capture (some windows may be cloaked)
    if (!IsWindowVisible(hwnd)) {
        LOG_WARNING("create_window_thumbnail: window is not visible");
        return QPixmap();
    }

    // One-shot, stable approach: grab window directly from the screen.
    // This avoids creating a temporary DWM host window (which can flicker) and avoids Qt widget grab issues.
    if (auto* screen = QGuiApplication::primaryScreen()) {
        QPixmap grabbed = screen->grabWindow((WId)hwnd);
        if (!grabbed.isNull()) {
            return grabbed.scaled(width, height, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        LOG_WARNING("create_window_thumbnail: QScreen::grabWindow(hwnd) returned null, fallback to PrintWindow");
    }

    // Fallback to PrintWindow
    HTHUMBNAIL thumbnail;
    HRESULT hr = DwmRegisterThumbnail((HWND)this->winId(), hwnd, &thumbnail);
    if (FAILED(hr)) {
        LOG_WARNING("DwmRegisterThumbnail failed with HRESULT: 0x" + QString::number(hr, 16).toStdString() + ", fallback to PrintWindow");
    } else {
        // Get thumbnail properties
        DWM_THUMBNAIL_PROPERTIES props = {};
        SIZE sourceSize = {};
        
        // Query source size
        hr = DwmQueryThumbnailSourceSize(thumbnail, &sourceSize);
        if (FAILED(hr)) {
            LOG_WARNING("DwmQueryThumbnailSourceSize failed with HRESULT: 0x" + QString::number(hr, 16).toStdString());
            DwmUnregisterThumbnail(thumbnail);
        } else {
            // Set up thumbnail properties
            props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY;
            props.fSourceClientAreaOnly = TRUE;
            props.fVisible = TRUE;
            props.opacity = 255;
            
            // Calculate destination rect
            QSize destSize(width, height);
            QRectF destRect = QRectF(0, 0, destSize.width(), destSize.height());
            
            // Adjust to maintain aspect ratio
            qreal aspectRatio = static_cast<qreal>(sourceSize.cx) / sourceSize.cy;
            if (destRect.width() / destRect.height() > aspectRatio) {
                destRect.setWidth(destRect.height() * aspectRatio);
            } else {
                destRect.setHeight(destRect.width() / aspectRatio);
            }
            destRect.moveCenter(QRectF(0, 0, width, height).center());

            // Update thumbnail properties
            props.rcDestination = {
                static_cast<LONG>(destRect.left()),
                static_cast<LONG>(destRect.top()),
                static_cast<LONG>(destRect.right()),
                static_cast<LONG>(destRect.bottom())
            };
            
            hr = DwmUpdateThumbnailProperties(thumbnail, &props);
            if (FAILED(hr)) {
                LOG_WARNING("DwmUpdateThumbnailProperties failed with HRESULT: 0x" + QString::number(hr, 16).toStdString());
                DwmUnregisterThumbnail(thumbnail);
            } else {
                // Create a temporary widget to host the thumbnail
                QWidget* tempWidget = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint);
                tempWidget->setAttribute(Qt::WA_TranslucentBackground);
                tempWidget->setAttribute(Qt::WA_NoSystemBackground);
                tempWidget->setWindowOpacity(0.0);
                tempWidget->setFixedSize(width, height);
                tempWidget->move(-width - 100, -height - 100);
                tempWidget->show();
                QCoreApplication::processEvents();
                QThread::msleep(50);
                QPixmap pixmap = tempWidget->grab(QRect(0, 0, width, height));
                tempWidget->hide();
                delete tempWidget;
                DwmUnregisterThumbnail(thumbnail);

                if (!pixmap.isNull()) {
                    return pixmap;
                }
                LOG_WARNING("DWM thumbnail grab returned null, fallback to PrintWindow");
            }
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
        // HD preview: capture on-demand at (or above) preview label resolution, cache it, then scale once.
        QSize dstSize = preview_label_->size();
        int capW = std::max(1, dstSize.width());
        int capH = std::max(1, dstSize.height());

        // Capture at 2x for better sharpness when downscaling
        int captureW = capW * 2;
        int captureH = capH * 2;

        if (selected_target_->hd_preview.isNull()) {
            if (selected_target_->type == CaptureTarget::Type::WINDOW) {
                HWND hwnd = reinterpret_cast<HWND>(std::stoull(selected_target_->id));
                selected_target_->hd_preview = create_window_thumbnail(hwnd, captureW, captureH);
            } else {
                selected_target_->hd_preview = create_screen_thumbnail(QString::fromStdString(selected_target_->id), captureW, captureH);
            }
        }

        const QPixmap& src = selected_target_->hd_preview.isNull() ? selected_target_->thumbnail : selected_target_->hd_preview;
        if (!src.isNull()) {
            QPixmap scaled = src.scaled(dstSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            preview_label_->setPixmap(scaled);
            preview_label_->setText("");
        } else {
            preview_label_->setPixmap(QPixmap());
            preview_label_->setText("无法预览");
        }

        QString preview_text = QString("预览 - %1\n尺寸: %2x%3")
            .arg(selected_target_->type == CaptureTarget::Type::SCREEN ? "屏幕" : "窗口")
            .arg(selected_target_->size.width())
            .arg(selected_target_->size.height());
        preview_label_->setToolTip(preview_text);
    }
}

void ScreenCaptureSelector::update_thumbnails() {
    // This function is now disabled to ensure one-shot thumbnail capture.
}

// Start an async thumbnail update for index if not updating and cooldown passed
void ScreenCaptureSelector::start_async_thumbnail_update(int index) {
    // This function is now disabled to ensure one-shot thumbnail capture.
    (void)index;
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