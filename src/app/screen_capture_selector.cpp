#include "app/screen_capture_selector.h"
#include "common/log.h"
#include <QApplication>
#include <QScreen>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QPixmap>
#include <QPainter>
#include <QWindow>
#include <QThread>
#include <algorithm>
#include <windows.h>
#include <dwmapi.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")

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
    refresh_targets();
}

ScreenCaptureSelector::~ScreenCaptureSelector() {
}

void ScreenCaptureSelector::setup_ui() {
    setWindowTitle("选择捕获目标");
    setMinimumSize(900, 280);
    setMaximumHeight(280);
    setStyleSheet(
        "QDialog { background-color: #1a1a1a; color: #ffffff; }"
        "QWidget { background-color: #1a1a1a; color: #ffffff; }"
        "QLabel { color: #ffffff; }"
        "QPushButton { background-color: #333333; color: #ffffff; border: 1px solid #444444; border-radius: 4px; padding: 8px 16px; }"
        "QPushButton:hover { background-color: #444444; }"
        "QPushButton:pressed { background-color: #555555; }"
        "QPushButton:disabled { background-color: #222222; color: #666666; }"
    );

    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(20, 15, 20, 15);
    main_layout->setSpacing(12);

    // Header
    auto* header_layout = new QHBoxLayout();
    auto* title_label = new QLabel("选择要共享的屏幕或窗口");
    title_label->setStyleSheet("font-weight: bold; font-size: 16px; color: #ffffff;");
    header_layout->addWidget(title_label);
    header_layout->addStretch();

    refresh_button_ = new QPushButton("刷新");
    refresh_button_->setMaximumWidth(80);
    connect(refresh_button_, &QPushButton::clicked, this, &ScreenCaptureSelector::refresh_targets);
    header_layout->addWidget(refresh_button_);
    main_layout->addLayout(header_layout);

    // Scroll area for horizontal thumbnails
    auto* scroll_area = new QScrollArea();
    scroll_area->setWidgetResizable(true);
    scroll_area->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll_area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_area->setFixedHeight(150);
    scroll_area->setStyleSheet(
        "QScrollArea { background-color: #1a1a1a; border: none; }"
        "QScrollArea > QWidget > QWidget { background-color: #1a1a1a; }"
        "QScrollBar:horizontal { background-color: #2a2a2a; height: 8px; border-radius: 4px; }"
        "QScrollBar:horizontal::handle { background-color: #444444; border-radius: 4px; min-width: 20px; }"
        "QScrollBar:horizontal::handle:hover { background-color: #555555; }"
        "QScrollBar:horizontal::add-line { width: 0px; }"
        "QScrollBar:horizontal::sub-line { width: 0px; }"
    );

    auto* scroll_content = new QWidget();
    scroll_content->setStyleSheet("background-color: #1a1a1a;");
    thumbnail_layout_ = new QHBoxLayout(scroll_content);
    thumbnail_layout_->setContentsMargins(0, 0, 0, 0);
    thumbnail_layout_->setSpacing(12);

    thumbnail_container_ = scroll_content;
    scroll_area->setWidget(scroll_content);
    main_layout->addWidget(scroll_area);

    // Bottom buttons
    auto* button_layout = new QHBoxLayout();
    button_layout->addStretch();

    ok_button_ = new QPushButton("确定");
    ok_button_->setEnabled(false);
    ok_button_->setMinimumWidth(100);
    connect(ok_button_, &QPushButton::clicked, this, &ScreenCaptureSelector::on_ok_clicked);

    cancel_button_ = new QPushButton("取消");
    cancel_button_->setMinimumWidth(100);
    connect(cancel_button_, &QPushButton::clicked, this, &ScreenCaptureSelector::on_cancel_clicked);

    button_layout->addWidget(ok_button_);
    button_layout->addSpacing(15);
    button_layout->addWidget(cancel_button_);
    main_layout->addLayout(button_layout);
}

void ScreenCaptureSelector::refresh_targets() {
    targets_.clear();

    // Clear existing thumbnail widgets
    QLayoutItem* item;
    while ((item = thumbnail_layout_->takeAt(0)) != nullptr) {
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    selected_target_ = nullptr;
    ok_button_->setEnabled(false);

    enumerate_screens();
    enumerate_windows();

    // Create horizontal thumbnail items
    const int thumb_width = 160;
    const int thumb_height = 90;

    for (int i = 0; i < static_cast<int>(targets_.size()); ++i) {
        const auto& target = targets_[i];

        // Create thumbnail widget
        auto* thumb_widget = create_thumbnail_widget(target, thumb_width, thumb_height);
        thumbnail_layout_->addWidget(thumb_widget);
    }

    // Force layout update
    thumbnail_container_->updateGeometry();
    QCoreApplication::processEvents();
}

void ScreenCaptureSelector::enumerate_screens() {
    // Get all screens
    const auto screens = QApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        QScreen* screen = screens[i];
        CaptureTarget target;
        target.type = CaptureTarget::Type::SCREEN;
        // Use Win32 device path (\\.\DISPLAY1, \\.\DISPLAY2) as id so WGC can bind the
        // correct monitor. QScreen::name() is often EDID/model text (e.g. "G2412WHI")
        // and cannot be matched by WGC. screen->handle() is QPlatformScreen*, NOT HMONITOR,
        // so we use MonitorFromPoint with the screen's center to get a valid HMONITOR.
        QPoint center = screen->geometry().center();
        HMONITOR hm = MonitorFromPoint(POINT{center.x(), center.y()}, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (hm && GetMonitorInfoW(hm, reinterpret_cast<LPMONITORINFO>(&mi))) {
            target.id = QString::fromWCharArray(mi.szDevice).toStdString();
        } else {
            target.id = screen->name().toStdString();
            LOG_WARNING("[ScreenCaptureSelector] Failed to get Win32 device name for screen: " +
                        target.name + ", using fallback id: " + target.id);
        }
        target.name = screen->name().toStdString();
        target.size = screen->size();

        // Create thumbnail for screen
        create_thumbnail_for_screen(target);

        targets_.push_back(std::move(target));
    }
}

void ScreenCaptureSelector::enumerate_windows() {
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

        targets_.push_back(std::move(target));
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
    const int thumb_width = 160;
    const int thumb_height = 90;
    target.thumbnail = create_screen_thumbnail(QString::fromStdString(target.id), thumb_width, thumb_height);

    // If thumbnail is null or invalid, create a placeholder
    if (target.thumbnail.isNull()) {
        target.thumbnail = QPixmap(thumb_width, thumb_height);
        target.thumbnail.fill(QColor(64, 64, 128)); // Dark blue for screens
    }
}

void ScreenCaptureSelector::create_thumbnail_for_window(CaptureTarget& target) {
    // Try to create a real thumbnail using PrintWindow or DWM
    HWND hwnd = reinterpret_cast<HWND>(std::stoull(target.id));
    const int thumb_width = 160;
    const int thumb_height = 90;
    target.thumbnail = create_window_thumbnail(hwnd, thumb_width, thumb_height);

    // If PrintWindow failed, create a placeholder
    if (target.thumbnail.isNull()) {
        target.thumbnail = QPixmap(thumb_width, thumb_height);
        target.thumbnail.fill(QColor(128, 64, 64)); // Dark red for windows
    }
}

QWidget* ScreenCaptureSelector::create_thumbnail_widget(const CaptureTarget& target, int width, int height) {
    auto* widget = new QWidget();
    widget->setFixedSize(width + 20, height + 50);
    widget->setCursor(Qt::PointingHandCursor);

    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    // Thumbnail label
    auto* thumb_label = new QLabel();
    thumb_label->setFixedSize(width, height);
    thumb_label->setScaledContents(true);
    thumb_label->setStyleSheet("border: 2px solid #333333; border-radius: 6px;");

    if (!target.thumbnail.isNull()) {
        thumb_label->setPixmap(target.thumbnail);
    } else {
        thumb_label->setStyleSheet("background-color: #2a2a2a; border: 2px solid #333333; border-radius: 6px;");
    }

    layout->addWidget(thumb_label);

    // Name label
    auto* name_label = new QLabel();
    name_label->setFixedSize(width, 40);
    name_label->setAlignment(Qt::AlignTop | Qt::AlignHCenter);

    QString name = QString::fromStdString(target.name);
    if (name.length() > 15) {
        name = name.left(12) + "...";
    }

    QString type_text = target.type == CaptureTarget::Type::SCREEN ? "屏幕" : "窗口";
    QString size_text = QString("%1x%2").arg(target.size.width()).arg(target.size.height());
    name_label->setText(QString("<span style='color: #aaaaaa; font-size: 11px;'>%1</span><br>"
                                "<span style='color: #ffffff; font-size: 12px;'>%2</span>")
                        .arg(type_text)
                        .arg(name + " " + size_text));
    name_label->setStyleSheet("background: transparent;");

    layout->addWidget(name_label);

    // Store target index for selection
    int idx = &target - &targets_[0];
    widget->setProperty("targetIndex", idx);

    // Make widget clickable via event filter
    widget->setAttribute(Qt::WA_Hover, true);
    widget->installEventFilter(this);

    return widget;
}

bool ScreenCaptureSelector::eventFilter(QObject* obj, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress) {
        if (auto* widget = qobject_cast<QWidget*>(obj)) {
            int idx = widget->property("targetIndex").toInt();
            if (idx >= 0) {
                on_thumbnail_clicked(idx);
                return true;
            }
        }
    }
    return QObject::eventFilter(obj, event);
}

void ScreenCaptureSelector::on_thumbnail_clicked(int index) {
    if (index < 0 || index >= static_cast<int>(targets_.size())) {
        return;
    }

    selected_target_ = &targets_[index];
    ok_button_->setEnabled(true);

    // Update visual selection state
    for (int i = 0; i < thumbnail_layout_->count(); ++i) {
        auto* item = thumbnail_layout_->itemAt(i);
        if (item && item->widget()) {
            auto* widget = qobject_cast<QWidget*>(item->widget());
            if (widget) {
                int idx = widget->property("targetIndex").toInt();
                bool is_selected = (idx == index);

                // Update border style to indicate selection
                auto* thumb_label = widget->findChild<QLabel*>();
                if (thumb_label) {
                    if (is_selected) {
                        thumb_label->setStyleSheet("border: 3px solid #5096FF; border-radius: 6px;");
                    } else {
                        thumb_label->setStyleSheet("border: 2px solid #333333; border-radius: 6px;");
                    }
                }
            }
        }
    }
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

void ScreenCaptureSelector::on_ok_clicked() {
    if (selected_target_) {
        accept();
    }
}

void ScreenCaptureSelector::on_cancel_clicked() {
    selected_target_ = nullptr;
    reject();
}

const CaptureTarget* ScreenCaptureSelector::get_selected_target() const {
    return selected_target_;
}

} // namespace live_assistant