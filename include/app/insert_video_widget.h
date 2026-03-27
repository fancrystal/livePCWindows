#ifndef INSERT_VIDEO_WIDGET_H
#define INSERT_VIDEO_WIDGET_H

#include <QDialog>
#include <QTableWidget>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <memory>
#include "media_pipeline/vlc_player.h"

namespace Ui {
class InsertVideoWidget;
}

class InsertVideoWidget : public QDialog {
    Q_OBJECT

public:
    explicit InsertVideoWidget(QWidget *parent = nullptr);
    ~InsertVideoWidget();

    // 设置直播间信息
    void setLiveInfo(const QString& sassUrl, const QString& userId, const QString& token, const QString& roomId);

    // 刷新插播视频列表
    void refreshVideoList();

signals:
    // 开始插播信号（包含循环播放设置）
    void startInsertVideo(const QString& fileId, const QString& fileName, bool loopEnabled);

protected:
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void onCloseButtonClicked();
    void onRefreshClicked();
    void onPlayClicked();
    void onStopPreviewClicked();
    void onStartInsertClicked();
    void onSearchTextChanged(const QString& text);
    void onItemSelectionChanged();
    void onFilesUpdated();
    void onDownloadProgress(const QString& fileId, int percent);
    void onDownloadFinished(const QString& fileId, bool success, const QString& message);
    void onErrorOccurred(const QString& fileId, const QString& message);
    void onPreviewEndReached();
    void onLoopCheckStateChanged(int row, int column);

private:
    void setupUI();
    void updateVideoTable();
    void updateButtonStates();
    void updateVlcPrewarmUi();
    void startVlcPrewarmMonitoring();
    QString formatDuration(qint64 durationMs) const;
    QString formatFileSize(qint64 bytes) const;
    QPixmap loadVideoThumbnail(const QString& coverUrl) const;

    // UI 控件
    QTableWidget* tableWidget_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* playButton_ = nullptr;
    QPushButton* stopPreviewButton_ = nullptr;
    QPushButton* startInsertButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* selectedLabel_ = nullptr;

    // 预览窗口控件
    QWidget* previewWidget_ = nullptr;
    QLabel* previewLabel_ = nullptr;

    // VLC 播放器
    std::unique_ptr<VlcPlayer> vlc_player_;
    bool is_previewing_ = false;
    QTimer* vlc_prewarm_timer_ = nullptr;
    bool vlc_prewarm_started_ = false;

    // 数据
    QString sass_url_;
    QString user_id_;
    QString token_;
    QString room_id_;
    QString selected_file_id_;
    bool is_initialized_ = false;

    // 下载进度条映射
    QMap<QString, QProgressBar*> progress_bars_;
};

#endif // INSERT_VIDEO_WIDGET_H
