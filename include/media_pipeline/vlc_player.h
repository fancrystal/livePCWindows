#ifndef VLC_PLAYER_H
#define VLC_PLAYER_H

#include <QObject>
#include <QWidget>
#include <QString>
#include <vlc/vlc.h>

class VlcPlayer : public QObject {
    Q_OBJECT
public:
    // 预热（只做 libvlc_instance 初始化），避免首次使用卡顿
    // 说明：初始化过程可能较慢（插件扫描），因此提供异步预热接口。
    static void prewarmAsync();
    static bool isPrewarmed();

    explicit VlcPlayer(QWidget *videoWidget, QObject *parent = nullptr);
    ~VlcPlayer();

    bool openFile(const QString &filePath);
    void play();
    void pause();
    void stop();
    void setPosition(float pos);
    void setVolume(int volume);
    void setPlaybackRate(float rate);
    bool isPlaying() const;
    qint64 duration() const;
    qint64 position() const;

    // 刷新视频输出设置（用于确保视频填满窗口）
    void refreshVideoOutput();

signals:
    void positionChanged(qint64 position);
    void durationChanged(qint64 duration);
    void stateChanged(int state);
    void endReached();

private:
    static void vlcEventCallback(const libvlc_event_t *event, void *userData);
    void handleVlcEvent(const libvlc_event_t *event);
    void setupVideoOutput();

    static libvlc_instance_t* ensureSharedInstance();

    libvlc_instance_t *m_vlcInstance;
    libvlc_media_player_t *m_vlcPlayer;
    libvlc_media_t *m_vlcMedia;
    QWidget *m_videoWidget;
};

#endif // VLC_PLAYER_H
