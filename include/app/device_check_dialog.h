#pragma once

#include <QDialog>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QCamera>
#include <QAudioSource>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QMediaCaptureSession>
#include <QVideoSink>
#include <QVideoWidget>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QElapsedTimer>
#include <QVector>
#include <QTimer>

// 前向声明（不在 live_assistant 命名空间内）
class QCustomPlot;

namespace Ui {
class DeviceCheckDialog;
}

namespace live_assistant {

/**
 * 设备检测对话框
 * 用于检测摄像头、麦克风、网络连接状态
 */
class DeviceCheckDialog : public QDialog {
    Q_OBJECT

public:
    explicit DeviceCheckDialog(QWidget *parent = nullptr);
    ~DeviceCheckDialog();

    struct DeviceCheckResult {
        bool cameraAvailable = false;
        bool microphoneAvailable = false;
        bool networkAvailable = false;
        double networkSpeedMbps = 0.0;
        QString selectedCameraId;
        QString selectedMicrophoneId;
    };

    DeviceCheckResult getCheckResult() const { return checkResult_; }

signals:
    void deviceCheckCompleted(const DeviceCheckResult& result);

private slots:
    void onCheckDevicesClicked();
    void onTestNetworkClicked();
    void onCameraChanged(int index);
    void onMicrophoneChanged(int index);
    void onNetworkTestFinished();
    void processAudioData(const QByteArray &audioData);
    void updateWaveform();
    void updateAudioSpectrum();

private:
    void initializeUI();
    void populateCameraList();
    void populateMicrophoneList();
    void startCameraPreview();
    void stopCameraPreview();
    void startMicrophoneTest();
    void stopMicrophoneTest();
    void testNetworkConnection();
    void setupAudioSpectrum();

    Ui::DeviceCheckDialog *ui;

    // 设备检测状态
    DeviceCheckResult checkResult_;
    bool isChecking_ = false;

    // 摄像头相关
    QComboBox* cameraComboBox_;
    QVideoWidget* videoWidget_;
    QCamera* camera_ = nullptr;
    QCameraDevice currentCameraDevice_;
    QMediaCaptureSession* captureSession_;
    QVideoSink* videoSink_;

    // 麦克风相关
    QComboBox* microphoneComboBox_;
    QLabel* microphoneStatusLabel_;
    QAudioSource* audioInput_ = nullptr;
    QAudioDevice currentMicDevice_;
    QAudioFormat audioFormat_;

    // 音频可视化
    QCustomPlot* waveformPlot_;
    QCustomPlot* spectrumPlot_;
    QVector<double> waveformData_;
    QVector<double> spectrumData_;
    int bufferSize_ = 256;
    QTimer* spectrumTimer_;

    // FFT (使用 void* 避免头文件依赖)
    void* fftCfg_ = nullptr;
    void* fftOut_ = nullptr;
    void* fftMem_ = nullptr;  // FFT配置内存块
    int fftSize_ = 1024;

    // 网络测试相关
    QPushButton* testNetworkButton_;
    QLabel* networkStatusLabel_;
    QNetworkAccessManager* networkManager_;
    QNetworkReply* networkReply_ = nullptr;
    qint64 receivedBytes_ = 0;
    QElapsedTimer downloadTimer_;
};

} // namespace live_assistant
