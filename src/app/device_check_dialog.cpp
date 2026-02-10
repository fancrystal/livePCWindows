#include "app/device_check_dialog.h"
#include "ui_device_check_dialog.h"
#include "common/log.h"
#include "qcustomplot.h"

// Include KissFFT headers (they already have extern "C" wrappers)
#include "kiss_fftr.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QTimer>
#include <QMessageBox>
#include <QMediaDevices>
#include <QCameraDevice>
#include <QAudioDevice>
#include <QVideoWidget>
#include <QPalette>
#include <QBrush>
#include <QtMath>
#include <QtMath>

namespace live_assistant {

// ==================== DeviceCheckDialog 实现 ====================

DeviceCheckDialog::DeviceCheckDialog(QWidget *parent)
    : QDialog(parent),
      ui(new Ui::DeviceCheckDialog),
      networkManager_(new QNetworkAccessManager(this)),
      captureSession_(nullptr),
      videoSink_(nullptr),
      waveformPlot_(nullptr),
      spectrumPlot_(nullptr),
      spectrumTimer_(new QTimer(this)),
      fftCfg_(nullptr),
      fftOut_(nullptr),
      fftSize_(1024),
      networkReply_(nullptr),
      receivedBytes_(0) {

    ui->setupUi(this);

    // 设置窗口属性
    setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowMaximizeButtonHint);
    setWindowTitle("设备检测");
    resize(900, 700);

    // 设置音频格式
    audioFormat_.setSampleRate(44100);
    audioFormat_.setChannelCount(1);
    audioFormat_.setSampleFormat(QAudioFormat::Int16);

    // 获取UI控件
    cameraComboBox_ = ui->cameraComboBox;
    microphoneComboBox_ = ui->microphoneComboBox;
    videoWidget_ = ui->videoWidget;
    waveformPlot_ = ui->waveformPlot;
    spectrumPlot_ = ui->spectrumPlot;
    microphoneStatusLabel_ = ui->microphoneStatusLabel;
    testNetworkButton_ = ui->testNetworkButton;
    networkStatusLabel_ = ui->networkStatusLabel;

    // 初始化音频数据
    bufferSize_ = 256;
    waveformData_.resize(bufferSize_, 0.0);
    spectrumData_.resize(bufferSize_, 0.0);

    // 初始化波形图
    if (waveformPlot_) {
        // 设置背景颜色透明
        waveformPlot_->setBackground(Qt::transparent);
        waveformPlot_->axisRect()->setBackground(Qt::transparent);
        waveformPlot_->addGraph();
        waveformPlot_->graph(0)->setPen(QPen(Qt::blue));
        waveformPlot_->xAxis->setRange(0, bufferSize_);
        waveformPlot_->yAxis->setRange(-1.0, 1.0);
        waveformPlot_->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
        waveformPlot_->plotLayout()->insertRow(0);
        waveformPlot_->plotLayout()->addElement(0, 0,
            new QCPTextElement(waveformPlot_, "音频波形图", QFont("sans", 10, QFont::Bold)));
    }

    // 初始化频谱图
    if (spectrumPlot_) {
        // 设置背景颜色透明
        spectrumPlot_->setBackground(Qt::transparent);
        spectrumPlot_->axisRect()->setBackground(Qt::transparent);
        spectrumPlot_->addGraph();
        spectrumPlot_->graph(0)->setPen(QPen(Qt::green));
        spectrumPlot_->graph(0)->setBrush(QBrush(QColor(0, 255, 0, 30)));
        spectrumPlot_->xAxis->setRange(0, bufferSize_);
        spectrumPlot_->yAxis->setRange(0, 1.0);
        spectrumPlot_->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
        spectrumPlot_->axisRect()->setupFullAxesBox(true);
        spectrumPlot_->plotLayout()->insertRow(0);
        spectrumPlot_->plotLayout()->addElement(0, 0,
            new QCPTextElement(spectrumPlot_, "音频频谱图", QFont("sans", 10, QFont::Bold)));
    }

    // 初始化FFT
    size_t fftMemSize = 0;
    kiss_fftr_alloc(fftSize_, 0, nullptr, &fftMemSize);
    fftMem_ = malloc(fftMemSize);
    if (!fftMem_) {
        LOG_ERROR("[DeviceCheck] Failed to allocate FFT memory");
        return;
    }
    fftCfg_ = kiss_fftr_alloc(fftSize_, 0, fftMem_, &fftMemSize);
    if (!fftCfg_) {
        LOG_ERROR("[DeviceCheck] Failed to initialize FFT config");
        free(fftMem_);
        fftMem_ = nullptr;
        return;
    }
    fftOut_ = malloc(sizeof(kiss_fft_cpx) * (fftSize_ / 2 + 1));
    if (!fftOut_) {
        LOG_ERROR("[DeviceCheck] Failed to allocate FFT output buffer");
        kiss_fftr_free(static_cast<kiss_fftr_cfg>(fftCfg_));
        fftCfg_ = nullptr;
        free(fftMem_);
        fftMem_ = nullptr;
        return;
    }

    // 调整频谱图范围
    if (spectrumPlot_) {
        spectrumPlot_->xAxis->setRange(0, fftSize_ / 2);
        spectrumPlot_->yAxis->setRange(0, 5.0);
    }

    // 连接信号槽
    connect(cameraComboBox_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DeviceCheckDialog::onCameraChanged);
    connect(microphoneComboBox_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DeviceCheckDialog::onMicrophoneChanged);
    connect(testNetworkButton_, &QPushButton::clicked,
            this, &DeviceCheckDialog::onTestNetworkClicked);
    connect(ui->checkDevicesButton, &QPushButton::clicked,
            this, &DeviceCheckDialog::onCheckDevicesClicked);
    connect(networkManager_, &QNetworkAccessManager::finished,
            this, &DeviceCheckDialog::onNetworkTestFinished);
    connect(spectrumTimer_, &QTimer::timeout,
            this, &DeviceCheckDialog::updateAudioSpectrum);

    // 启动定时器
    QTimer* plotTimer = new QTimer(this);
    connect(plotTimer, &QTimer::timeout, this, &DeviceCheckDialog::updateWaveform);
    plotTimer->start(50);
    spectrumTimer_->start(50);

    // 填充设备列表
    populateCameraList();
    populateMicrophoneList();

    // 默认选中第一个设备
    if (cameraComboBox_->count() > 0 && cameraComboBox_->itemText(0) != "未检测到摄像头") {
        cameraComboBox_->setCurrentIndex(0);
        onCameraChanged(0);
    }

    if (microphoneComboBox_->count() > 0 && microphoneComboBox_->itemText(0) != "未检测到麦克风") {
        microphoneComboBox_->setCurrentIndex(0);
        onMicrophoneChanged(0);
    }
}

DeviceCheckDialog::~DeviceCheckDialog() {
    stopCameraPreview();
    stopMicrophoneTest();

    // 释放FFT资源（注意顺序：先释放输出，再释放配置，最后释放内存块）
    if (fftOut_) {
        free(fftOut_);
        fftOut_ = nullptr;
    }
    if (fftCfg_) {
        kiss_fftr_free(static_cast<kiss_fftr_cfg>(fftCfg_));
        fftCfg_ = nullptr;
    }
    if (fftMem_) {
        free(fftMem_);
        fftMem_ = nullptr;
    }

    delete ui;
}

void DeviceCheckDialog::populateCameraList() {
    cameraComboBox_->clear();
    const QList<QCameraDevice> cameras = QMediaDevices::videoInputs();

    if (cameras.isEmpty()) {
        cameraComboBox_->addItem("未检测到摄像头");
        checkResult_.cameraAvailable = false;
    } else {
        for (const QCameraDevice &cam : cameras) {
            cameraComboBox_->addItem(cam.description(), QVariant::fromValue(cam.id()));
        }
        checkResult_.cameraAvailable = true;
        LOG_INFO("[DeviceCheck] Found " + std::to_string(cameras.size()) + " camera(s)");
    }
}

void DeviceCheckDialog::populateMicrophoneList() {
    microphoneComboBox_->clear();
    const QList<QAudioDevice> mics = QMediaDevices::audioInputs();

    if (mics.isEmpty()) {
        microphoneComboBox_->addItem("未检测到麦克风");
        checkResult_.microphoneAvailable = false;
    } else {
        for (const QAudioDevice &mic : mics) {
            microphoneComboBox_->addItem(mic.description(), QVariant::fromValue(mic.id()));
        }
        checkResult_.microphoneAvailable = true;
        LOG_INFO("[DeviceCheck] Found " + std::to_string(mics.size()) + " microphone(s)");
    }
}

void DeviceCheckDialog::onCameraChanged(int index) {
    if (index < 0 || index >= cameraComboBox_->count()) return;

    stopCameraPreview();

    QString cameraId = cameraComboBox_->itemData(index).toString();
    checkResult_.selectedCameraId = cameraId;

    if (cameraComboBox_->itemText(index) == "未检测到摄像头") {
        return;
    }

    // 查找对应的摄像头设备
    const QList<QCameraDevice> cameras = QMediaDevices::videoInputs();
    for (const QCameraDevice &cam : cameras) {
        if (cam.id() == cameraId) {
            currentCameraDevice_ = cam;
            break;
        }
    }

    startCameraPreview();
}

void DeviceCheckDialog::startCameraPreview() {
    if (currentCameraDevice_.isNull()) {
        LOG_WARNING("[DeviceCheck] No camera device selected");
        return;
    }

    camera_ = new QCamera(currentCameraDevice_, this);
    captureSession_ = new QMediaCaptureSession(this);
    captureSession_->setCamera(camera_);

    // 设置视频输出
    if (videoWidget_) {
        captureSession_->setVideoOutput(videoWidget_);
        videoWidget_->setVisible(true);
    }

    camera_->start();
    LOG_INFO("[DeviceCheck] Camera preview started: " + currentCameraDevice_.description().toStdString());
}

void DeviceCheckDialog::stopCameraPreview() {
    if (camera_) {
        camera_->stop();
        delete camera_;
        camera_ = nullptr;
    }
    if (captureSession_) {
        delete captureSession_;
        captureSession_ = nullptr;
    }
}

void DeviceCheckDialog::onMicrophoneChanged(int index) {
    if (index < 0 || index >= microphoneComboBox_->count()) return;

    stopMicrophoneTest();

    QString micId = microphoneComboBox_->itemData(index).toString();
    checkResult_.selectedMicrophoneId = micId;

    if (microphoneComboBox_->itemText(index) == "未检测到麦克风") {
        microphoneStatusLabel_->setText("❌ 麦克风不可用");
        return;
    }

    // 查找对应的麦克风设备
    const QList<QAudioDevice> mics = QMediaDevices::audioInputs();
    for (const QAudioDevice &mic : mics) {
        if (mic.id() == micId) {
            currentMicDevice_ = mic;
            break;
        }
    }

    startMicrophoneTest();
}

void DeviceCheckDialog::startMicrophoneTest() {
    if (currentMicDevice_.isNull()) {
        LOG_WARNING("[DeviceCheck] No microphone device selected");
        return;
    }

    // 检查设备是否支持该格式
    if (!currentMicDevice_.isFormatSupported(audioFormat_)) {
        audioFormat_ = currentMicDevice_.preferredFormat();
        LOG_WARNING("[DeviceCheck] Using preferred audio format");
    }

    audioInput_ = new QAudioSource(currentMicDevice_, audioFormat_, this);

    // 创建音频IO设备来接收音频数据
    class AudioCaptureDevice : public QIODevice {
    public:
        explicit AudioCaptureDevice(DeviceCheckDialog* parent) : QIODevice(parent), parent_(parent) {
            open(QIODevice::WriteOnly);
        }

    protected:
        qint64 readData(char* data, qint64 maxlen) override {
            Q_UNUSED(data);
            Q_UNUSED(maxlen);
            return 0;
        }

        qint64 writeData(const char* data, qint64 len) override {
            if (len <= 0) return 0;
            QByteArray d(data, len);
            // 直接调用父类的处理函数
            parent_->processAudioData(d);
            return len;
        }

    private:
        DeviceCheckDialog* parent_;
    };

    AudioCaptureDevice* audioCaptureDevice = new AudioCaptureDevice(this);
    audioInput_->start(audioCaptureDevice);
    microphoneStatusLabel_->setText("✅ 麦克风正常工作");

    LOG_INFO("[DeviceCheck] Microphone test started: " + currentMicDevice_.description().toStdString());
}

void DeviceCheckDialog::stopMicrophoneTest() {
    if (audioInput_) {
        audioInput_->stop();
        delete audioInput_;
        audioInput_ = nullptr;
    }
}

void DeviceCheckDialog::processAudioData(const QByteArray& audioData) {
    const int sampleSize = audioFormat_.bytesPerSample();
    const int numSamples = audioData.size() / sampleSize;

    if (numSamples <= 0) {
        return;
    }

    // 准备FFT输入缓冲区
    std::vector<kiss_fft_scalar> fftIn(fftSize_, 0.0f);
    int samplesToProcess = qMin(numSamples, fftSize_);

    // 处理不同的采样格式
    if (audioFormat_.sampleFormat() == QAudioFormat::Int16) {
        const int16_t* samples = reinterpret_cast<const int16_t*>(audioData.constData());

        // 填充波形数据
        std::fill(waveformData_.begin(), waveformData_.end(), 0.0);
        int waveSamples = qMin(bufferSize_, numSamples);
        for (int i = 0; i < waveSamples; ++i) {
            waveformData_[i] = static_cast<double>(samples[i]) / 32768.0;
        }

        // 填充FFT输入（归一化到[-1, 1]范围）
        for (int i = 0; i < samplesToProcess; ++i) {
            fftIn[i] = static_cast<kiss_fft_scalar>(samples[i]) / 32768.0f;
        }
    } else if (audioFormat_.sampleFormat() == QAudioFormat::Float) {
        const float* samples = reinterpret_cast<const float*>(audioData.constData());

        // 填充波形数据
        std::fill(waveformData_.begin(), waveformData_.end(), 0.0);
        int waveSamples = qMin(bufferSize_, numSamples);
        for (int i = 0; i < waveSamples; ++i) {
            waveformData_[i] = static_cast<double>(samples[i]);
        }

        // 填充FFT输入
        for (int i = 0; i < samplesToProcess; ++i) {
            fftIn[i] = static_cast<kiss_fft_scalar>(samples[i]);
        }
    }

    // 执行FFT
    if (fftCfg_ && fftOut_) {
        kiss_fftr_cfg cfg = static_cast<kiss_fftr_cfg>(fftCfg_);
        kiss_fft_cpx* out = static_cast<kiss_fft_cpx*>(fftOut_);
        kiss_fftr(cfg, fftIn.data(), out);
    }

    // 计算频谱幅度并存储
    spectrumData_.resize(fftSize_ / 2, 0.0);
    if (fftOut_) {
        kiss_fft_cpx* out = static_cast<kiss_fft_cpx*>(fftOut_);
        for (int i = 0; i < fftSize_ / 2; ++i) {
            double magnitude = std::sqrt(out[i].r * out[i].r + out[i].i * out[i].i);
            spectrumData_[i] = magnitude;
        }
    }
}

void DeviceCheckDialog::updateWaveform() {
    if (!waveformPlot_ || waveformData_.isEmpty()) return;

    QVector<double> x(bufferSize_), y(bufferSize_);
    for (int i = 0; i < bufferSize_; ++i) {
        x[i] = i;
        y[i] = waveformData_[i];
    }

    waveformPlot_->graph(0)->setData(x, y);
    waveformPlot_->replot();
}

void DeviceCheckDialog::updateAudioSpectrum() {
    if (!spectrumPlot_ || spectrumData_.isEmpty()) return;

    QVector<double> x(spectrumData_.size()), y(spectrumData_.size());
    for (int i = 0; i < spectrumData_.size(); ++i) {
        x[i] = i;
        y[i] = spectrumData_[i];
    }

    spectrumPlot_->graph(0)->setData(x, y);
    spectrumPlot_->replot();
}

void DeviceCheckDialog::setupAudioSpectrum() {
    // 音频频谱设置已在构造函数中完成
}

void DeviceCheckDialog::onTestNetworkClicked() {
    testNetworkButton_->setEnabled(false);
    testNetworkButton_->setText("测试中...");
    networkStatusLabel_->setText("正在测试网络连接...");

    testNetworkConnection();
}

void DeviceCheckDialog::testNetworkConnection() {
    QString testUrl = "https://stor.lxi-tech.com/ceshi/%E5%B8%8C%E7%89%B9%E5%8B%921M.mp4";

    QNetworkRequest request(testUrl);
    networkReply_ = networkManager_->get(request);

    receivedBytes_ = 0;
    downloadTimer_.start();

    connect(networkReply_, &QNetworkReply::readyRead, [this]() {
        receivedBytes_ += networkReply_->bytesAvailable();
        networkReply_->readAll();
    });

    LOG_INFO("[DeviceCheck] Network test started");
}

void DeviceCheckDialog::onNetworkTestFinished() {
    if (!networkReply_) return;

    testNetworkButton_->setEnabled(true);
    testNetworkButton_->setText("测试网络");

    if (networkReply_->error() == QNetworkReply::NoError) {
        double elapsedSec = downloadTimer_.elapsed() / 1000.0;
        if (elapsedSec > 0) {
            double speedBps = (receivedBytes_ * 8.0) / elapsedSec;
            double speedMbps = speedBps / (1000.0 * 1000.0);

            checkResult_.networkSpeedMbps = speedMbps;
            checkResult_.networkAvailable = (speedMbps > 0.5);

            if (checkResult_.networkAvailable) {
                networkStatusLabel_->setText(
                    QString("✅ 网络正常 (速度: %1 Mbps)").arg(speedMbps, 0, 'f', 2));
            } else {
                networkStatusLabel_->setText(
                    QString("⚠️ 网络较慢 (速度: %1 Mbps)").arg(speedMbps, 0, 'f', 2));
            }

            LOG_INFO("[DeviceCheck] Network test completed: " + std::to_string(speedMbps) + " Mbps");
        }
    } else {
        checkResult_.networkAvailable = false;
        networkStatusLabel_->setText("❌ 网络连接失败: " + networkReply_->errorString());
        LOG_ERROR("[DeviceCheck] Network test failed: " + networkReply_->errorString().toStdString());
    }

    networkReply_->deleteLater();
    networkReply_ = nullptr;
}

void DeviceCheckDialog::onCheckDevicesClicked() {
    // 执行完整的设备检查
    checkResult_.cameraAvailable = (cameraComboBox_->count() > 0 &&
                                     cameraComboBox_->itemText(0) != "未检测到摄像头");
    checkResult_.microphoneAvailable = (microphoneComboBox_->count() > 0 &&
                                        microphoneComboBox_->itemText(0) != "未检测到麦克风");

    // 如果还没有测试网络，自动测试
    if (testNetworkButton_->isEnabled() && checkResult_.networkSpeedMbps == 0.0) {
        onTestNetworkClicked();
    }

    // 延迟一点等待网络测试完成
    QTimer::singleShot(1000, this, [this]() {
        // 显示检测结果
        QString resultMessage;
        int passCount = 0;

        if (checkResult_.cameraAvailable) passCount++;
        if (checkResult_.microphoneAvailable) passCount++;
        if (checkResult_.networkAvailable) passCount++;

        if (passCount == 3) {
            resultMessage = "✅ 所有设备检测通过！";
            QMessageBox::information(this, "设备检测", resultMessage);
        } else {
            resultMessage = QString("⚠️ 设备检测完成 (%1/3 通过)").arg(passCount);
            QMessageBox::warning(this, "设备检测", resultMessage + "\n\n请确保所有设备正常工作。");
        }

        emit deviceCheckCompleted(checkResult_);
    });
}

} // namespace live_assistant
