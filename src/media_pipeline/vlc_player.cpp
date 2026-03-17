#include "media_pipeline/vlc_player.h"
#include "common/log.h"
#include <QDebug>
#include <QUrl>
#include <QTimer>
#include <QFileInfo>
#include <QCoreApplication>
#include <QtConcurrent/QtConcurrent>

#include <atomic>
#include <memory>
#include <mutex>

namespace {

std::once_flag g_vlc_init_once;
std::atomic_bool g_vlc_ready{false};
std::mutex g_vlc_mutex;
std::shared_ptr<libvlc_instance_t> g_vlc_instance;

libvlc_instance_t* createVlcInstance() {
    // 获取插件目录路径
    QString pluginPath = QCoreApplication::applicationDirPath() + "/plugins";
    qDebug() << "VLC plugin path:" << pluginPath;

    const QByteArray pluginArg = QString("--plugin-path=%1").arg(pluginPath).toLocal8Bit();
    const char* vlcArgs[] = {
        "--no-xlib",
        "--no-audio-time-stretch",
        "--no-video-title-show",  // 不显示视频标题
        "--quiet",                // 减少控制台输出
        "--verbose=0",
        pluginArg.constData(),
    };

    libvlc_instance_t* inst = libvlc_new(sizeof(vlcArgs) / sizeof(vlcArgs[0]), vlcArgs);
    if (!inst) {
        LOG_ERROR("Failed to create VLC instance - possibly missing plugins or wrong VLC version");
        // 尝试不使用插件路径初始化（兜底）
        const char* vlcArgsNoPlugin[] = {
            "--no-xlib",
            "--no-audio-time-stretch",
            "--quiet",
            "--verbose=0",
        };
        inst = libvlc_new(sizeof(vlcArgsNoPlugin) / sizeof(vlcArgsNoPlugin[0]), vlcArgsNoPlugin);
        if (!inst) {
            LOG_ERROR("Failed to create VLC instance even without plugin path");
            return nullptr;
        }
        LOG_WARNING("VLC instance created without explicit plugin path");
    }

    return inst;
}

}  // namespace

void VlcPlayer::prewarmAsync() {
    // 只触发一次后台初始化；后续调用无副作用
    if (g_vlc_ready.load()) return;

    auto future = QtConcurrent::run([]() {
        (void)VlcPlayer::ensureSharedInstance();
    });
    (void)future;
}

bool VlcPlayer::isPrewarmed() {
    return g_vlc_ready.load();
}

libvlc_instance_t* VlcPlayer::ensureSharedInstance() {
    std::call_once(g_vlc_init_once, []() {
        libvlc_instance_t* inst = createVlcInstance();
        if (!inst) {
            g_vlc_ready.store(false);
            return;
        }

        std::lock_guard<std::mutex> lock(g_vlc_mutex);
        g_vlc_instance.reset(inst, [](libvlc_instance_t* p) {
            if (p) libvlc_release(p);
        });
        g_vlc_ready.store(true);
    });

    std::lock_guard<std::mutex> lock(g_vlc_mutex);
    return g_vlc_instance.get();
}

VlcPlayer::VlcPlayer(QWidget *videoWidget, QObject *parent)
    : QObject(parent), m_vlcInstance(nullptr), 
      m_vlcPlayer(nullptr), m_vlcMedia(nullptr),
      m_videoWidget(videoWidget)
{
    // 使用全局共享的 libvlc_instance，避免每次创建播放器都触发插件扫描
    m_vlcInstance = ensureSharedInstance();
    if (!m_vlcInstance) {
        LOG_ERROR("VLC shared instance not available");
        return;
    }

    m_vlcPlayer = libvlc_media_player_new(m_vlcInstance);
    if (!m_vlcPlayer) {
        LOG_ERROR("Failed to create VLC media player");
        m_vlcInstance = nullptr;
        return;
    }

    // 设置事件回调
    libvlc_event_manager_t *em = libvlc_media_player_event_manager(m_vlcPlayer);
    if (em) {
        libvlc_event_attach(em, libvlc_MediaPlayerPositionChanged, vlcEventCallback, this);
        libvlc_event_attach(em, libvlc_MediaPlayerTimeChanged, vlcEventCallback, this);
        libvlc_event_attach(em, libvlc_MediaPlayerLengthChanged, vlcEventCallback, this);
        libvlc_event_attach(em, libvlc_MediaPlayerPlaying, vlcEventCallback, this);
        libvlc_event_attach(em, libvlc_MediaPlayerPaused, vlcEventCallback, this);
        libvlc_event_attach(em, libvlc_MediaPlayerStopped, vlcEventCallback, this);
        libvlc_event_attach(em, libvlc_MediaPlayerEndReached, vlcEventCallback, this);
    }

    // 延迟设置视频输出，确保窗口完全初始化
    QTimer::singleShot(100, this, [this]() {
        setupVideoOutput();
    });
}

VlcPlayer::~VlcPlayer()
{
    // 分离事件回调（防止回调访问已释放的对象）
    if (m_vlcPlayer) {
        libvlc_event_manager_t *em = libvlc_media_player_event_manager(m_vlcPlayer);
        if (em) {
            libvlc_event_detach(em, libvlc_MediaPlayerPositionChanged, vlcEventCallback, this);
            libvlc_event_detach(em, libvlc_MediaPlayerTimeChanged, vlcEventCallback, this);
            libvlc_event_detach(em, libvlc_MediaPlayerLengthChanged, vlcEventCallback, this);
            libvlc_event_detach(em, libvlc_MediaPlayerPlaying, vlcEventCallback, this);
            libvlc_event_detach(em, libvlc_MediaPlayerPaused, vlcEventCallback, this);
            libvlc_event_detach(em, libvlc_MediaPlayerStopped, vlcEventCallback, this);
            libvlc_event_detach(em, libvlc_MediaPlayerEndReached, vlcEventCallback, this);
        }
        // 直接释放，不调用stop以避免可能的阻塞
        libvlc_media_player_release(m_vlcPlayer);
        m_vlcPlayer = nullptr;
    }

    // 注意：m_vlcInstance 是共享实例，不在此处释放
    m_vlcInstance = nullptr;

    if (m_vlcMedia) {
        libvlc_media_release(m_vlcMedia);
        m_vlcMedia = nullptr;
    }
}

void VlcPlayer::setupVideoOutput()
{
    if (!m_vlcPlayer || !m_videoWidget) {
        LOG_ERROR("VLC Player or Video Widget is null");
        return;
    }

    // 确保视频窗口句柄有效
    WId winId = m_videoWidget->winId();
    if (!winId) {
        LOG_ERROR("Invalid window ID");
        return;
    }
    LOG_INFO(QString("Setting up video output for widget: %1").arg((quintptr)winId).toStdString());

    // 跨平台设置视频输出
#if defined(Q_OS_WIN)
    libvlc_media_player_set_hwnd(m_vlcPlayer, (void*)winId);
#elif defined(Q_OS_MAC)
    libvlc_media_player_set_nsobject(m_vlcPlayer, (void*)winId);
#else // Linux and other platforms
    libvlc_media_player_set_xwindow(m_vlcPlayer, winId);
#endif

    // 设置自适应缩放模式：视频将铺满整个窗口
    libvlc_video_set_scale(m_vlcPlayer, 0.0f);  // 0 = 自适应铺满

    // 关键：设置宽高比为 NULL，让 VLC 忽略原始宽高比，拉伸填充窗口
    // 这样可以消除黑边，视频会完全铺满预览窗口
    libvlc_video_set_aspect_ratio(m_vlcPlayer, NULL);

    // 清除裁剪设置，确保没有任何裁剪
    libvlc_video_set_crop_geometry(m_vlcPlayer, NULL);

    // 设置为全屏自适应（忽略原始视频尺寸）
    libvlc_video_set_scale(m_vlcPlayer, 0.0f);

    qDebug() << "Video Widget WinID:" << winId;
    qDebug() << "Video Widget Geometry:" << m_videoWidget->geometry();
    qDebug() << "Video Widget isVisible:" << m_videoWidget->isVisible();
}

bool VlcPlayer::openFile(const QString &filePath)
{
    if (!m_vlcInstance || !m_vlcPlayer) {
        LOG_ERROR("VLC Instance or Player not initialized");
        return false;
    }

    if (m_vlcMedia) {
        libvlc_media_release(m_vlcMedia);
        m_vlcMedia = nullptr;
    }

    // 检查文件是否存在
    QFileInfo fileInfo(filePath);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        LOG_ERROR(QString("File does not exist or is not a regular file: %1").arg(filePath).toStdString());
        return false;
    }

    LOG_INFO(QString("Attempting to open file: %1").arg(filePath).toStdString());
    LOG_INFO(QString("File size: %1 bytes").arg(fileInfo.size()).toStdString());

    QUrl fileUrl = QUrl::fromLocalFile(filePath);
    QString fileUri = fileUrl.toEncoded();
    QByteArray uriBytes = fileUri.toUtf8();
    const char* cUri = uriBytes.constData();
    LOG_INFO(QString("Using URI to open file: %1").arg(uriBytes.constData()).toStdString());

    m_vlcMedia = libvlc_media_new_location(m_vlcInstance, cUri);
    if (!m_vlcMedia) {
        LOG_ERROR(QString("Failed to create media from URI: %1").arg(fileUri).toStdString());
        return false;
    }

    libvlc_media_player_set_media(m_vlcPlayer, m_vlcMedia);

    // 延迟设置视频输出（再次确保）
    QTimer::singleShot(50, this, [this]() {
        setupVideoOutput();
    });

    return true;
}

void VlcPlayer::play()
{
    if (m_vlcPlayer) {
        // 确保视频输出设置
        setupVideoOutput();
        libvlc_media_player_play(m_vlcPlayer);
        LOG_INFO("Started playing video");
    } else {
        LOG_ERROR("VLC Player is null, cannot play");
    }
}

void VlcPlayer::pause()
{
    if (m_vlcPlayer) {
        libvlc_media_player_pause(m_vlcPlayer);
        LOG_INFO("Paused video playback");
    }
}

void VlcPlayer::stop()
{
    if (m_vlcPlayer) {
        libvlc_media_player_pause(m_vlcPlayer);
        libvlc_media_player_stop(m_vlcPlayer);
        LOG_INFO("Stopped video playback");
    }
}

void VlcPlayer::setPosition(float pos)
{
    if (m_vlcPlayer) {
        libvlc_media_player_set_position(m_vlcPlayer, pos);
    }
}

void VlcPlayer::setVolume(int volume)
{
    if (m_vlcPlayer) {
        // 确保音量在有效范围内 (0-100)
        int safeVolume = qBound(0, volume, 100);
        libvlc_audio_set_volume(m_vlcPlayer, safeVolume);
        LOG_INFO(QString("Set volume: %1").arg(safeVolume).toStdString());
    }
}

void VlcPlayer::setPlaybackRate(float rate)
{
    if (m_vlcPlayer) {
        libvlc_media_player_set_rate(m_vlcPlayer, rate);
        LOG_INFO(QString("Set playback rate: %1x").arg(rate).toStdString());
    }
}

bool VlcPlayer::isPlaying() const
{
    if (m_vlcPlayer) {
        return libvlc_media_player_is_playing(m_vlcPlayer);
    }
    return false;
}

qint64 VlcPlayer::duration() const
{
    if (m_vlcPlayer) {
        return libvlc_media_player_get_length(m_vlcPlayer);
    }
    return 0;
}

qint64 VlcPlayer::position() const
{
    if (m_vlcPlayer) {
        return libvlc_media_player_get_time(m_vlcPlayer);
    }
    return 0;
}

void VlcPlayer::refreshVideoOutput()
{
    if (m_vlcPlayer && m_videoWidget) {
        // 重新设置视频输出参数，确保视频填满窗口
        libvlc_video_set_scale(m_vlcPlayer, 0.0f);
        libvlc_video_set_aspect_ratio(m_vlcPlayer, NULL);
        libvlc_video_set_crop_geometry(m_vlcPlayer, NULL);
    }
}

void VlcPlayer::vlcEventCallback(const libvlc_event_t *event, void *userData)
{
    VlcPlayer *player = static_cast<VlcPlayer*>(userData);
    if (player) {
        player->handleVlcEvent(event);
    }
}

void VlcPlayer::handleVlcEvent(const libvlc_event_t *event)
{
    switch (event->type) {
    case libvlc_MediaPlayerPositionChanged:
        emit positionChanged(position());
        break;
    case libvlc_MediaPlayerTimeChanged:
        emit positionChanged(position());
        break;
    case libvlc_MediaPlayerLengthChanged:
        emit durationChanged(duration());
        break;
    case libvlc_MediaPlayerPlaying:
        emit stateChanged(1);
        break;
    case libvlc_MediaPlayerPaused:
        emit stateChanged(0);
        break;
    case libvlc_MediaPlayerStopped:
        emit stateChanged(-1);
        break;
    case libvlc_MediaPlayerEndReached:
        emit endReached();
        break;
    default:
        break;
    }
}
