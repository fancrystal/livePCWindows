/**
 * @file qsv_encoder.cpp
 * @brief QSV 硬件编码器实现
 *
 * 参考 OBS Studio obs-qsv11 插件：
 * - QSV_Encoder.cpp
 * - QSV_Encoder_Internal.cpp
 * - obs-qsv11.c
 */

#include "encoder/qsv_encoder.h"
#include "encoder/qsv_device.h"
#include "common/log.h"

#include <algorithm>
#include <cstring>

// ============================================================================
// 常量
// ============================================================================

constexpr uint32_t SPS_BUFFER_SIZE = 1024;
constexpr uint32_t PPS_BUFFER_SIZE = 1024;

// ============================================================================
// 任务池
// ============================================================================

struct TaskInfo {
    mfxBitstream mfx_bs;
    mfxSyncPoint syncp;
    bool in_use;

    TaskInfo() : syncp(nullptr), in_use(false) {
        memset(&mfx_bs, 0, sizeof(mfx_bs));
    }
};

// ============================================================================
// QSVEncoder 实现
// ============================================================================

namespace live_assistant {

class QSVEncoderImpl : public QSVEncoder {
public:
    QSVEncoderImpl(const QSVEncoderParams& params);
    ~QSVEncoderImpl() override;

    bool initialize() override;
    void shutdown() override;
    bool reconfigure(const QSVEncoderParams& params) override;

    bool encode(
        const uint8_t* yuv_data, size_t size, uint64_t timestamp,
        std::vector<QSVEncodeResult>& results) override;

    bool encode_texture(
        void* texture_handle, uint64_t lock_key, uint64_t* next_key,
        uint64_t timestamp, std::vector<QSVEncodeResult>& results) override;

    bool drain(std::vector<QSVEncodeResult>& results) override;
    void force_keyframe() override;

    const QSVEncoderParams& params() const override { return params_; }
    bool get_extradata(std::vector<uint8_t>& sps,
                       std::vector<uint8_t>& pps) const override;

    bool supports_texture_encoding() const override {
        return params_.use_texture_alloc;
    }

    mfxFrameAllocator* frame_allocator() override;
    int num_surfaces() const override { return num_surfaces_; }

private:
    // 初始化参数
    bool init_params();
    bool allocate_surfaces();
    void free_surfaces();

    // 获取空闲任务/表面索引
    int get_free_task();
    int get_free_surface();

    // 内部编码
    bool encode_surface(int surface_idx, int task_idx, uint64_t timestamp,
                        std::vector<QSVEncodeResult>& results);

    // 刷新编码器
    bool flush_output(int task_idx, std::vector<QSVEncodeResult>& results);

    // 获取编码参数
    mfxStatus get_video_param();

    // 成员变量
    QSVEncoderParams params_;

    // VPL 编码器
    mfxSession session_ = nullptr;
    MFXVideoENCODE* encoder_ = nullptr;

    // 编码参数
    mfxVideoParam video_params_;
    mfxExtCodingOptionSPSPPS sps_pps_option_;

    // 帧表面
    mfxFrameSurface1** surfaces_ = nullptr;
    int num_surfaces_ = 0;
    mfxFrameAllocResponse alloc_response_;

    // 任务池
    TaskInfo* task_pool_ = nullptr;
    int task_pool_size_ = 0;
    int task_head_ = 0;
    int task_tail_ = 0;

    // 编码控制
    mfxEncodeCtrl enc_ctrl_;
    bool force_keyframe_ = false;

    // 头数据
    std::vector<uint8_t> sps_data_;
    std::vector<uint8_t> pps_data_;

    // 设备管理器
    QSVDeviceManager* device_manager_ = nullptr;
    bool initialized_ = false;
};

// ============================================================================
// 工厂函数
// ============================================================================

std::unique_ptr<QSVEncoder> QSVEncoder::create(const QSVEncoderParams& params) {
    return std::make_unique<QSVEncoderImpl>(params);
}

// ============================================================================
// QSVEncoderImpl 实现
// ============================================================================

QSVEncoderImpl::QSVEncoderImpl(const QSVEncoderParams& params)
    : params_(params), task_pool_(nullptr), surfaces_(nullptr) {

    memset(&video_params_, 0, sizeof(video_params_));
    memset(&sps_pps_option_, 0, sizeof(sps_pps_option_));
    memset(&enc_ctrl_, 0, sizeof(enc_ctrl_));
}

QSVEncoderImpl::~QSVEncoderImpl() {
    shutdown();
}

bool QSVEncoderImpl::initialize() {
    if (initialized_) {
        LOG_WARNING("[QSV] Encoder already initialized");
        return true;
    }

    LOG_INFO("[QSV] Initializing encoder: " +
             std::to_string(params_.width) + "x" + std::to_string(params_.height) +
             " @ " + std::to_string(params_.frame_rate_num) + "fps, " +
             "bitrate=" + std::to_string(params_.target_bitrate));

    // 1. 获取设备管理器单例
    device_manager_ = &QSVDeviceManager::instance();

    // 2. 初始化设备
    mfxStatus sts = device_manager_->initialize(0);
    if (sts != MFX_ERR_NONE) {
        LOG_ERROR("[QSV] Failed to initialize device manager: " + std::to_string(sts));
        return false;
    }

    // 3. 获取 VPL Session
    session_ = device_manager_->session();
    if (!session_) {
        LOG_ERROR("[QSV] No VPL session");
        return false;
    }

    // 4. 创建编码器
    mfxU32 codec_id;
    switch (params_.codec) {
        case QSVCodec::AVC:
            codec_id = MFX_CODEC_AVC;
            break;
        case QSVCodec::HEVC:
            codec_id = MFX_CODEC_HEVC;
            break;
        case QSVCodec::AV1:
            codec_id = MFX_CODEC_AV1;
            break;
        default:
            codec_id = MFX_CODEC_AVC;
    }

    encoder_ = new MFXVideoENCODE(session_);

    // 5. 初始化参数
    if (!init_params()) {
        LOG_ERROR("[QSV] Failed to init params");
        delete encoder_;
        encoder_ = nullptr;
        return false;
    }

    // 6. 查询编码器
    sts = encoder_->Query(&video_params_, &video_params_);
    if (sts != MFX_ERR_NONE && sts != MFX_WRN_INCOMPATIBLE_VIDEO_PARAM) {
        LOG_ERROR("[QSV] Query failed: " + std::to_string(sts));
        delete encoder_;
        encoder_ = nullptr;
        return false;
    }

    // 7. 分配表面
    if (!allocate_surfaces()) {
        LOG_ERROR("[QSV] Failed to allocate surfaces");
        delete encoder_;
        encoder_ = nullptr;
        return false;
    }

    // 8. 初始化编码器
    sts = encoder_->Init(&video_params_);
    if (sts != MFX_ERR_NONE) {
        LOG_ERROR("[QSV] Init failed: " + std::to_string(sts));
        free_surfaces();
        delete encoder_;
        encoder_ = nullptr;
        return false;
    }

    // 9. 获取编码参数
    sts = get_video_param();
    if (sts != MFX_ERR_NONE) {
        LOG_WARNING("[QSV] get_video_param warning: " + std::to_string(sts));
    }

    // 10. 提取 SPS/PPS
    if (!sps_data_.empty() && !pps_data_.empty()) {
        LOG_INFO("[QSV] Got extradata: SPS=" + std::to_string(sps_data_.size()) +
                 " bytes, PPS=" + std::to_string(pps_data_.size()) + " bytes");
    }

    // 11. 任务池大小 = 表面数 + 异步深度
    task_pool_size_ = num_surfaces_ + params_.async_depth;
    task_pool_ = new TaskInfo[task_pool_size_];

    // 分配每个任务的码流缓冲区
    uint32_t buffer_size = params_.width * params_.height * 3 / 2;  // NV12 大小
    for (int i = 0; i < task_pool_size_; i++) {
        task_pool_[i].mfx_bs.MaxLength = buffer_size;
        task_pool_[i].mfx_bs.Data = new uint8_t[buffer_size];
    }

    initialized_ = true;
    LOG_INFO("[QSV] Encoder initialized: surfaces=" + std::to_string(num_surfaces_) +
             ", tasks=" + std::to_string(task_pool_size_));

    return true;
}

void QSVEncoderImpl::shutdown() {
    if (!initialized_) return;

    LOG_INFO("[QSV] Shutting down encoder...");

    // 刷新编码器
    if (encoder_) {
        encoder_->Close();
    }

    // 释放任务池
    if (task_pool_) {
        for (int i = 0; i < task_pool_size_; i++) {
            if (task_pool_[i].mfx_bs.Data) {
                delete[] task_pool_[i].mfx_bs.Data;
                task_pool_[i].mfx_bs.Data = nullptr;
            }
        }
        delete[] task_pool_;
        task_pool_ = nullptr;
    }

    // 释放表面
    free_surfaces();

    // 释放编码器
    delete encoder_;
    encoder_ = nullptr;

    // 释放设备管理器
    if (device_manager_) {
        device_manager_->shutdown();
        device_manager_ = nullptr;
    }

    initialized_ = false;
    LOG_INFO("[QSV] Encoder shut down");
}

bool QSVEncoderImpl::init_params() {
    memset(&video_params_, 0, sizeof(video_params_));

    // Codec
    switch (params_.codec) {
        case QSVCodec::AVC:
            video_params_.mfx.CodecId = MFX_CODEC_AVC;
            break;
        case QSVCodec::HEVC:
            video_params_.mfx.CodecId = MFX_CODEC_HEVC;
            break;
        case QSVCodec::AV1:
            video_params_.mfx.CodecId = MFX_CODEC_AV1;
            break;
    }

    // 分辨率
    video_params_.mfx.FrameInfo.Width = static_cast<mfxU16>(
        (params_.width + 31) & ~31);  // 16 字节对齐
    video_params_.mfx.FrameInfo.Height = static_cast<mfxU16>(
        (params_.height + 31) & ~31);
    video_params_.mfx.FrameInfo.CropW = static_cast<mfxU16>(params_.width);
    video_params_.mfx.FrameInfo.CropH = static_cast<mfxU16>(params_.height);
    video_params_.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    video_params_.mfx.FrameInfo.ChromaFormat = 1;  // 4:2:0

    // 帧率
    video_params_.mfx.FrameInfo.FrameRateExtN = params_.frame_rate_num;
    video_params_.mfx.FrameInfo.FrameRateExtD = params_.frame_rate_den;

    // 码率控制
    switch (params_.rate_control) {
        case QSVRateControl::CBR:
            video_params_.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
            video_params_.mfx.TargetKbps = params_.target_bitrate / 1000;
            break;
        case QSVRateControl::VBR:
            video_params_.mfx.RateControlMethod = MFX_RATECONTROL_VBR;
            video_params_.mfx.TargetKbps = params_.target_bitrate / 1000;
            if (params_.max_bitrate > 0) {
                video_params_.mfx.MaxKbps = params_.max_bitrate / 1000;
            }
            break;
        case QSVRateControl::CQP:
            video_params_.mfx.RateControlMethod = MFX_RATECONTROL_CQP;
            video_params_.mfx.QPI = params_.qp_init;
            video_params_.mfx.QPP = params_.qp_init;
            video_params_.mfx.QPB = params_.qp_init;
            break;
        case QSVRateControl::ICQ:
            video_params_.mfx.RateControlMethod = MFX_RATECONTROL_ICQ;
            video_params_.mfx.ICQQuality = params_.qp_init;
            break;
        default:
            video_params_.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
            video_params_.mfx.TargetKbps = params_.target_bitrate / 1000;
    }

    // GOP
    video_params_.mfx.GopPicSize = params_.gop_size;
    video_params_.mfx.GopRefDist = params_.b_frames ? 2 : 1;
    video_params_.mfx.GopOptFlag = MFX_GOP_STRICT;
    video_params_.mfx.KeyframeCtrl = MFX_KEY_INTERVAL_STrict;

    // 质量
    video_params_.mfx.TargetUsage = params_.target_usage;
    video_params_.mfx.NumRefFrame = params_.num_ref_frames;
    video_params_.mfx.NumSlice = 1;

    // 异步深度
    video_params_.AsyncDepth = params_.async_depth;

    // B 帧
    video_params_.mfx.MaxDecFrameBuffering = 1;
    video_params_.mfx.MaxEncodedFrameBufferSize = 0;

    // 扩展参数
    std::vector<mfxExtBuffer*> ext_buffers;

    // SPS/PPS
    sps_pps_option_.Header.BufferId = MFX_EXTBUFF_CODING_OPTION_SPSPPS;
    sps_pps_option_.Header.BufferSz = sizeof(sps_pps_option_);
    sps_pps_option_.SPSBufferSize = SPS_BUFFER_SIZE;
    sps_pps_option_.PPSBufferSize = PPS_BUFFER_SIZE;
    sps_pps_option_.RepeatedHeaders = params_.b_frames ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
    ext_buffers.push_back(&sps_pps_option_);

    if (!ext_buffers.empty()) {
        video_params_.NumExtParam = static_cast<mfxU16>(ext_buffers.size());
        video_params_.ExtParam = ext_buffers.data();
    }

    return true;
}

bool QSVEncoderImpl::allocate_surfaces() {
    // 查询需要的表面数
    mfxFrameAllocRequest request = {};
    request.Info = video_params_.mfx.FrameInfo;
    request.Type = MFX_MEMTYPE_FROM_ENCODE | MFX_MEMTYPE_VIDEO_MEMORY_PROCESSOR_TARGET;
    request.NumFrameSuggested = 0;
    request.NumFrameMin = 0;

    mfxStatus sts = encoder_->QueryIOSurf(&video_params_, &request);
    if (sts != MFX_ERR_NONE) {
        LOG_ERROR("[QSV] QueryIOSurf failed: " + std::to_string(sts));
        return false;
    }

    // 增加异步深度
    request.NumFrameSuggested += params_.async_depth;
    request.NumFrameMin = request.NumFrameSuggested;

    // 分配表面
    mfxFrameAllocator* allocator = device_manager_->allocator();
    sts = allocator->Alloc(allocator->pthis, &request, &alloc_response_);
    if (sts != MFX_ERR_NONE) {
        LOG_ERROR("[QSV] Surface allocation failed: " + std::to_string(sts));
        return false;
    }

    num_surfaces_ = alloc_response_.NumFrameActual;

    // 创建表面数组
    surfaces_ = new mfxFrameSurface1*[num_surfaces_];
    for (int i = 0; i < num_surfaces_; i++) {
        surfaces_[i] = new mfxFrameSurface1();
        memset(surfaces_[i], 0, sizeof(mfxFrameSurface1));
        memcpy(&surfaces_[i]->Info, &request.Info, sizeof(mfxFrameInfo));
        surfaces_[i]->Data.MemId = alloc_response_.mids[i];
        surfaces_[i]->Data.TimeStamp = 0;
        surfaces_[i]->Data.FrameOrder = 0;
        surfaces_[i]->Data.LockFlags = 0;
        surfaces_[i]->Data.NumExtParam = 0;
        surfaces_[i]->Data.ExtParam = nullptr;
    }

    LOG_INFO("[QSV] Allocated " + std::to_string(num_surfaces_) + " surfaces");
    return true;
}

void QSVEncoderImpl::free_surfaces() {
    if (surfaces_) {
        for (int i = 0; i < num_surfaces_; i++) {
            if (surfaces_[i]) {
                delete surfaces_[i];
                surfaces_[i] = nullptr;
            }
        }
        delete[] surfaces_;
        surfaces_ = nullptr;
    }

    if (alloc_response_.mids) {
        mfxFrameAllocator* allocator = device_manager_->allocator();
        if (allocator) {
            allocator->Free(allocator->pthis, &alloc_response_);
        }
        alloc_response_.mids = nullptr;
    }

    num_surfaces_ = 0;
}

bool QSVEncoderImpl::reconfigure(const QSVEncoderParams& params) {
    if (!initialized_ || !encoder_) {
        return false;
    }

    params_ = params;
    init_params();

    mfxStatus sts = encoder_->Reset(&video_params_);
    if (sts != MFX_ERR_NONE) {
        LOG_ERROR("[QSV] Reset failed: " + std::to_string(sts));
        return false;
    }

    return true;
}

bool QSVEncoderImpl::encode(
    const uint8_t* yuv_data, size_t size, uint64_t timestamp,
    std::vector<QSVEncodeResult>& results) {

    if (!initialized_) return false;

    // 锁定一帧
    int surf_idx = get_free_surface();
    if (surf_idx < 0) {
        // 等待并重试
        return false;
    }

    mfxFrameSurface1* surface = surfaces_[surf_idx];

    // 锁定表面并复制数据
    mfxFrameData frame_data;
    mfxStatus sts = device_manager_->lock_frame(surface->Data.MemId, &frame_data);
    if (sts != MFX_ERR_NONE) {
        LOG_ERROR("[QSV] Lock frame failed");
        return false;
    }

    // 复制 Y
    uint8_t* dst_y = frame_data.Y;
    uint8_t* src_y = const_cast<uint8_t*>(yuv_data);
    int row_size = params_.width;
    int height = params_.height;

    for (int y = 0; y < height; y++) {
        memcpy(dst_y + y * frame_data.Pitch, src_y + y * row_size, row_size);
    }

    // 复制 UV
    uint8_t* dst_uv = frame_data.U;
    uint8_t* src_uv = const_cast<uint8_t*>(yuv_data) + height * row_size;
    int uv_height = height / 2;
    int uv_row_size = row_size;  // NV12 UV 平面与 Y 平面行宽相同

    for (int y = 0; y < uv_height; y++) {
        memcpy(dst_uv + y * frame_data.Pitch, src_uv + y * uv_row_size, uv_row_size);
    }

    device_manager_->unlock_frame(surface->Data.MemId, &frame_data);

    // 设置时间戳
    surface->Data.TimeStamp = timestamp;

    // 获取任务
    int task_idx = get_free_task();
    if (task_idx < 0) {
        return false;
    }

    // 编码
    bool ok = encode_surface(surf_idx, task_idx, timestamp, results);
    task_pool_[task_idx].in_use = false;

    return ok;
}

bool QSVEncoderImpl::encode_texture(
    void* texture_handle, uint64_t lock_key, uint64_t* next_key,
    uint64_t timestamp, std::vector<QSVEncodeResult>& results) {

    if (!initialized_ || !params_.use_texture_alloc) {
        return false;
    }

    // 获取空闲表面
    int surf_idx = get_free_surface();
    if (surf_idx < 0) {
        LOG_WARNING("[QSV] No free surface for texture encode");
        return false;
    }

    int task_idx = get_free_task();
    if (task_idx < 0) {
        return false;
    }

    mfxFrameSurface1* surface = surfaces_[surf_idx];

    // 使用 KeyedMutex 复制纹理
    EncoderTexture tex = {};
    tex.handle = static_cast<HANDLE>(texture_handle);
    tex.texture = nullptr;  // 将通过 handle 打开

    QSVMemId mem_id;
    mem_id.surface = nullptr;  // 需要从 mid 获取
    mem_id.stage = nullptr;

    // 从 mid 获取表面
    auto* mids = reinterpret_cast<std::vector<mfxMemId*>*>(alloc_response_.mids);
    if (mids && surf_idx < static_cast<int>(mids->size())) {
        // 这需要根据实际实现调整
    }

    // 简化版本：使用默认的内存复制
    // TODO: 实现真正的纹理复制

    surface->Data.TimeStamp = timestamp;

    bool ok = encode_surface(surf_idx, task_idx, timestamp, results);
    task_pool_[task_idx].in_use = false;

    return ok;
}

bool QSVEncoderImpl::encode_surface(
    int surface_idx, int task_idx, uint64_t timestamp,
    std::vector<QSVEncodeResult>& results) {

    TaskInfo& task = task_pool_[task_idx];
    mfxFrameSurface1* surface = surfaces_[surface_idx];

    surface->Data.TimeStamp = timestamp;

    // 编码控制
    mfxEncodeCtrl ctrl = {};
    if (force_keyframe_) {
        ctrl.FrameType = MFX_FRAMETYPE_I | MFX_FRAMETYPE_REF;
        force_keyframe_ = false;
    }

    // 重置码流缓冲区
    task.mfx_bs.DataLength = 0;
    task.mfx_bs.DataOffset = 0;
    task.syncp = nullptr;

    // 编码
    mfxStatus sts;
    for (int retry = 0; retry < 100; retry++) {
        sts = encoder_->EncodeFrameAsync(
            ctrl.NumExtParam ? &ctrl : nullptr,
            surface,
            &task.mfx_bs,
            &task.syncp);

        if (sts == MFX_WRN_DEVICE_BUSY) {
            // 设备忙，等待
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        break;
    }

    if (sts != MFX_ERR_NONE && sts != MFX_ERR_INCOMPATIBLE_SESSION) {
        LOG_ERROR("[QSV] EncodeFrameAsync failed: " + std::to_string(sts));
        return false;
    }

    if (sts == MFX_ERR_INCOMPATIBLE_SESSION) {
        // 需要刷新
        return flush_output(task_idx, results);
    }

    // 等待编码完成
    if (task.syncp) {
        sts = MFXVideoCORE_SyncOperation(session_, task.syncp, 10000);
        if (sts != MFX_ERR_NONE) {
            LOG_ERROR("[QSV] Sync failed: " + std::to_string(sts));
            return false;
        }
    }

    return flush_output(task_idx, results);
}

bool QSVEncoderImpl::flush_output(int task_idx, std::vector<QSVEncodeResult>& results) {
    TaskInfo& task = task_pool_[task_idx];

    if (task.mfx_bs.DataLength == 0) {
        return true;
    }

    QSVEncodeResult result;
    result.timestamp = task.mfx_bs.TimeStamp;
    result.data.resize(task.mfx_bs.DataLength);
    memcpy(result.data.data(), task.mfx_bs.Data + task.mfx_bs.DataOffset,
           task.mfx_bs.DataLength);

    // 判断帧类型
    mfxU32 ft = task.mfx_bs.FrameType;
    result.is_keyframe = (ft & MFX_FRAMETYPE_I) != 0;
    result.frame_type = ft;

    results.push_back(result);

    // 重置
    task.mfx_bs.DataLength = 0;

    return true;
}

bool QSVEncoderImpl::drain(std::vector<QSVEncodeResult>& results) {
    if (!initialized_) return false;

    // 发送空帧刷新编码器
    mfxStatus sts = encoder_->EncodeFrameAsync(nullptr, nullptr, nullptr, nullptr);

    // 处理所有待输出的帧
    for (int i = 0; i < task_pool_size_; i++) {
        if (task_pool_[i].mfx_bs.DataLength > 0) {
            flush_output(i, results);
        }
    }

    return true;
}

void QSVEncoderImpl::force_keyframe() {
    force_keyframe_ = true;
}

bool QSVEncoderImpl::get_extradata(std::vector<uint8_t>& sps,
                                   std::vector<uint8_t>& pps) const {
    sps = sps_data_;
    pps = pps_data_;
    return true;
}

int QSVEncoderImpl::get_free_task() {
    for (int i = 0; i < task_pool_size_; i++) {
        int idx = (task_head_ + i) % task_pool_size_;
        if (!task_pool_[idx].in_use) {
            task_head_ = (task_head_ + 1) % task_pool_size_;
            task_pool_[idx].in_use = true;
            return idx;
        }
    }
    return -1;
}

int QSVEncoderImpl::get_free_surface() {
    for (int i = 0; i < num_surfaces_; i++) {
        if (surfaces_[i] && surfaces_[i]->Data.Locked == 0) {
            return i;
        }
    }
    return -1;
}

mfxStatus QSVEncoderImpl::get_video_param() {
    memset(&video_params_, 0, sizeof(video_params_));

    std::vector<mfxExtBuffer*> ext_buffers;

    // SPS/PPS
    sps_pps_option_.Header.BufferId = MFX_EXTBUFF_CODING_OPTION_SPSPPS;
    sps_pps_option_.Header.BufferSz = sizeof(sps_pps_option_);
    ext_buffers.push_back(&sps_pps_option_);

    video_params_.NumExtParam = static_cast<mfxU16>(ext_buffers.size());
    video_params_.ExtParam = ext_buffers.data();

    mfxStatus sts = encoder_->GetVideoParam(&video_params_);
    if (sts == MFX_ERR_NONE) {
        // 提取 SPS/PPS
        if (sps_pps_option_.SPSBufferSize > 0 && sps_pps_option_.SPSBuffer) {
            sps_data_.resize(sps_pps_option_.SPSBufferSize);
            memcpy(sps_data_.data(), sps_pps_option_.SPSBuffer, sps_data_.size());
        }
        if (sps_pps_option_.PPSBufferSize > 0 && sps_pps_option_.PPSBuffer) {
            pps_data_.resize(sps_pps_option_.PPSBufferSize);
            memcpy(pps_data_.data(), sps_pps_option_.PPSBuffer, pps_data_.size());
        }
    }

    return sts;
}

mfxFrameAllocator* QSVEncoderImpl::frame_allocator() {
    if (!device_manager_) return nullptr;
    return device_manager_->allocator();
}

} // namespace live_assistant
