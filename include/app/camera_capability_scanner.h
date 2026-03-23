#pragma once

#include <string>
#include <vector>
#include <memory>
#include <QObject>
#include "scene_manager/icapture_source.h"

namespace live_assistant {

/**
 * 摄像头能力扫描器
 * 用于查询指定摄像头支持的所有分辨率、帧率和像素格式组合
 */
class CameraCapabilityScanner : public QObject {
    Q_OBJECT
public:
    explicit CameraCapabilityScanner(QObject* parent = nullptr);
    ~CameraCapabilityScanner();

    /**
     * 查询指定摄像头的支持能力（同步）
     * @param device_id 摄像头设备ID (dshow name)
     * @param capture_mode 采集模式 (FFMPEG 或 OPENCV)
     * @return 摄像头能力集
     */
    CameraCapabilities query_capabilities(const std::string& device_id,
                                          CaptureMode capture_mode = CaptureMode::FFMPEG);

    /**
     * 异步查询摄像头能力
     * 查询完成后发射 capabilities_ready 信号
     */
    void query_capabilities_async(const std::string& device_id,
                                   CaptureMode capture_mode = CaptureMode::FFMPEG);

    /**
     * 获取默认的摄像头能力列表 (用于UI初始化)
     */
    static CameraCapabilities get_default_capabilities();

signals:
    // 异步查询完成信号
    void capabilities_ready(const QString& device_id, const CameraCapabilities& caps);
};

} // namespace live_assistant