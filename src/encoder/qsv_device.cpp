/**
 * @file qsv_device.cpp
 * @brief QSV D3D11 设备管理和帧分配器实现
 *
 * 参考 OBS Studio obs-qsv11 插件：
 * - common_directx11.cpp
 * - common_utils_windows.cpp
 */

#include "encoder/qsv_device.h"
#include "common/log.h"

#include <util/windows/com-formatter.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/pipe.h>
#include <util/config-file.h>

#include <intrin.h>
#include <inttypes.h>
#include <map>
#include <set>

// ============================================================================
// 全局变量
// ============================================================================

static constexpr size_t MAX_ADAPTERS = 10;
static live_assistant::AdapterInfo g_adapters[MAX_ADAPTERS];
static size_t g_adapter_count = MAX_ADAPTERS;
static size_t g_adapter_index = 0;

// ============================================================================
// QSVMemId 析构
// ============================================================================

namespace live_assistant {

QSVMemId::~QSVMemId() {
    if (surface) {
        surface->Release();
        surface = nullptr;
    }
    if (stage) {
        stage->Release();
        stage = nullptr;
    }
}

} // namespace live_assistant

// ============================================================================
// 辅助函数
// ============================================================================

#define MSDK_CHECK_RESULT(P, X, ERR) \
    do { \
        if ((X) > (P)) { \
            LOG_ERROR("[QSV] Check result failed: " #X " > " #P); \
            return ERR; \
        } \
    } while(0)

#define MSDK_CHECK_POINTER(P, ERR) \
    do { \
        if (!(P)) { \
            LOG_ERROR("[QSV] Null pointer: " #P); \
            return ERR; \
        } \
    } while(0)

#define MSDK_ALIGN32(X) (((mfxU32)((X) + 31)) & (~(mfxU32)31))

static void check_adapters_impl(live_assistant::AdapterInfo* adapters, size_t* adapter_count) {
    // TODO: 实现适配器检测逻辑
    // 可以通过调用 obs-qsv-test.exe 或直接枚举 DXGI 适配器来检测
    // 目前使用简化实现
    *adapter_count = 1;
    adapters[0].is_intel = true;
    adapters[0].is_dgpu = false;
    adapters[0].supports_av1 = true;
    adapters[0].supports_hevc = true;
}

static void util_cpuid(int cpuinfo[4], int flags) {
#ifdef _MSC_VER
    __cpuid(cpuinfo, flags);
#else
    __get_cpuid(flags, reinterpret_cast<unsigned int*>(&cpuinfo[0]),
                reinterpret_cast<unsigned int*>(&cpuinfo[1]),
                reinterpret_cast<unsigned int*>(&cpuinfo[2]),
                reinterpret_cast<unsigned int*>(&cpuinfo[3]));
#endif
}

// ============================================================================
// QSVDeviceManager 实现
// ============================================================================

namespace live_assistant {

QSVDeviceManager& QSVDeviceManager::instance() {
    static QSVDeviceManager inst;
    return inst;
}

QSVDeviceManager::QSVDeviceManager()
    : allocator_({}) {
    allocator_.pthis = this;
    allocator_.Alloc = static_alloc;
    allocator_.Free = static_free;
    allocator_.Lock = static_lock;
    allocator_.Unlock = static_unlock;
    allocator_.GetHDL = static_get_hdl;
}

QSVDeviceManager::~QSVDeviceManager() {
    shutdown();
}

IDXGIAdapter* QSVDeviceManager::get_intel_adapter(mfxSession session) {
    if (!session) return nullptr;

    mfxU32 adapter_num = 0;
    mfxIMPL impl;

    MFXQueryIMPL(session, &impl);

    mfxIMPL base_impl = MFX_IMPL_BASETYPE(impl);

    // 获取对应的适配器编号
    struct {
        mfxIMPL impl;
        mfxU32 adapter_id;
    } impl_types[] = {
        {MFX_IMPL_HARDWARE, 0},
        {MFX_IMPL_HARDWARE2, 1},
        {MFX_IMPL_HARDWARE3, 2},
        {MFX_IMPL_HARDWARE4, 3}
    };

    for (size_t i = 0; i < sizeof(impl_types) / sizeof(impl_types[0]); i++) {
        if (impl_types[i].impl == base_impl) {
            adapter_num = impl_types[i].adapter_id;
            break;
        }
    }

    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory2),
                                    reinterpret_cast<void**>(dxgi_factory_.put()));
    if (FAILED(hr)) {
        LOG_ERROR("[QSV] Failed to create DXGI factory");
        return nullptr;
    }

    IDXGIAdapter* adapter = nullptr;
    hr = dxgi_factory_->EnumAdapters(adapter_num, &adapter);
    if (FAILED(hr)) {
        LOG_ERROR("[QSV] Failed to enum adapter " + std::to_string(adapter_num));
        return nullptr;
    }

    return adapter;
}

mfxStatus QSVDeviceManager::create_d3d11_device(IDXGIAdapter* adapter) {
    static D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL feature_level_out;

    // 使用 D3D_DRIVER_TYPE_UNKNOWN（当指定 adapter 时必须用这个）
    HRESULT hr = D3D11CreateDevice(
        adapter,
        D3D_DRIVER_TYPE_UNKNOWN,  // 关键！不能是 HARDWARE
        nullptr,
        0,  // D3D11_CREATE_DEVICE_BGRA_SUPPORT 如果需要
        feature_levels,
        sizeof(feature_levels) / sizeof(feature_levels[0]),
        D3D11_SDK_VERSION,
        &d3d_device_,
        &feature_level_out,
        &d3d_context_
    );

    if (FAILED(hr)) {
        LOG_ERROR("[QSV] D3D11CreateDevice failed, hr=0x" +
                  std::to_string(static_cast<uint32_t>(hr)));
        return MFX_ERR_DEVICE_FAILED;
    }

    LOG_INFO("[QSV] D3D11 device created, feature level: " +
             std::to_string(static_cast<int>(feature_level_out)));

    // 启用多线程保护
    CComQIPtr<ID3D10Multithread> multithread(d3d_context_);
    if (multithread) {
        multithread->SetMultithreadProtected(TRUE);
        LOG_INFO("[QSV] D3D11 multithread protection enabled");
    } else {
        LOG_WARNING("[QSV] ID3D10Multithread not available");
    }

    device_handle_ = static_cast<mfxHDL>(d3d_device_);

    return MFX_ERR_NONE;
}

mfxStatus QSVDeviceManager::initialize(int adapter_idx) {
    if (initialized_) {
        refcount_++;
        LOG_INFO("[QSV] Device manager already initialized, refcount=" +
                 std::to_string(refcount_));
        return MFX_ERR_NONE;
    }

    LOG_INFO("[QSV] Initializing QSV device manager...");

    // 1. 检测适配器
    check_adapters_impl(g_adapters, &g_adapter_count);

    // 2. 选择适配器（优先 Intel）
    int selected_adapter = adapter_idx;
    if (!g_adapters[adapter_idx].is_intel) {
        for (size_t i = 0; i < g_adapter_count; i++) {
            if (g_adapters[i].is_intel) {
                selected_adapter = static_cast<int>(i);
                break;
            }
        }
    }

    // 3. 加载 VPL
    loader_ = MFXLoad();
    if (!loader_) {
        LOG_ERROR("[QSV] MFXLoad failed");
        return MFX_ERR_DEVICE_FAILED;
    }

    // 4. 配置 VPL
    mfxConfig config = MFXCreateConfig(loader_);
    if (!config) {
        LOG_ERROR("[QSV] MFXCreateConfig failed");
        MFXUnload(loader_);
        return MFX_ERR_DEVICE_FAILED;
    }

    mfxVariant impl;
    impl.Type = MFX_VARIANT_TYPE_U32;

    // 要求硬件实现
    impl.Data.U32 = MFX_IMPL_TYPE_HARDWARE;
    MFXSetConfigFilterProperty(config,
        reinterpret_cast<const mfxU8*>("mfxImplDescription.Impl"), impl);

    // 要求 Intel
    impl.Data.U32 = INTEL_VENDOR_ID;
    MFXSetConfigFilterProperty(config,
        reinterpret_cast<const mfxU8*>("mfxImplDescription.VendorID"), impl);

    // 要求 D3D11 加速
    impl.Data.U32 = MFX_ACCEL_MODE_VIA_D3D11;
    MFXSetConfigFilterProperty(config,
        reinterpret_cast<const mfxU8*>("mfxImplDescription.AccelerationMode"), impl);

    // 5. 创建 VPL Session
    mfxStatus sts = MFXCreateSession(loader_, selected_adapter, &session_);
    if (sts != MFX_ERR_NONE) {
        LOG_ERROR("[QSV] MFXCreateSession failed: " + std::to_string(sts));
        MFXUnload(loader_);
        return sts;
    }

    LOG_INFO("[QSV] VPL Session created on adapter " + std::to_string(selected_adapter));

    // 6. 创建 D3D11 设备
    adapter_ = get_intel_adapter(session_);
    if (!adapter_) {
        LOG_ERROR("[QSV] Failed to get Intel adapter");
        MFXClose(session_);
        MFXUnload(loader_);
        return MFX_ERR_DEVICE_FAILED;
    }

    sts = create_d3d11_device(adapter_);
    if (sts != MFX_ERR_NONE) {
        LOG_ERROR("[QSV] Failed to create D3D11 device");
        MFXClose(session_);
        MFXUnload(loader_);
        return sts;
    }

    // 7. 将 D3D11 设备注册到 VPL（关键步骤！）
    sts = MFXVideoCORE_SetHandle(session_, QSV_DEVICE_MGR_TYPE, device_handle_);
    if (sts != MFX_ERR_NONE) {
        LOG_ERROR("[QSV] MFXVideoCORE_SetHandle failed: " + std::to_string(sts));
        shutdown();
        return sts;
    }

    LOG_INFO("[QSV] D3D11 device registered to VPL");

    // 8. 设置帧分配器
    setup_allocator();
    sts = MFXVideoCORE_SetFrameAllocator(session_, &allocator_);
    if (sts != MFX_ERR_NONE) {
        LOG_WARNING("[QSV] MFXVideoCORE_SetFrameAllocator failed: " + std::to_string(sts));
        // 不致命，继续
    } else {
        LOG_INFO("[QSV] Frame allocator registered");
    }

    initialized_ = true;
    refcount_ = 1;

    LOG_INFO("[QSV] Device manager initialized successfully");
    return MFX_ERR_NONE;
}

void QSVDeviceManager::shutdown() {
    if (!initialized_) return;

    refcount_--;
    if (refcount_ > 0) {
        LOG_INFO("[QSV] Device manager refcount=" + std::to_string(refcount_) +
                 ", keeping alive");
        return;
    }

    LOG_INFO("[QSV] Shutting down QSV device manager...");

    // 清理帧响应
    for (auto& pair : encode_responses_) {
        free_internal(&pair.second);
    }
    encode_responses_.clear();

    // 关闭 D3D11
    d3d_context_ = nullptr;
    d3d_device_ = nullptr;

    if (adapter_) {
        adapter_->Release();
        adapter_ = nullptr;
    }

    if (dxgi_factory_) {
        dxgi_factory_->Release();
        dxgi_factory_ = nullptr;
    }

    // 关闭 VPL
    if (session_) {
        MFXClose(session_);
        session_ = nullptr;
    }

    if (loader_) {
        MFXUnload(loader_);
        loader_ = nullptr;
    }

    device_handle_ = nullptr;
    initialized_ = false;

    LOG_INFO("[QSV] QSV device manager shut down");
}

void QSVDeviceManager::setup_allocator() {
    // allocator_ 已在构造函数中初始化
}

ID3D11Texture2D* QSVDeviceManager::create_shared_texture(
    UINT width, UINT height, DXGI_FORMAT format, HANDLE* shared_handle) {

    if (!d3d_device_) return nullptr;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    CComPtr<ID3D11Texture2D> texture;
    HRESULT hr = d3d_device_->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(hr)) {
        LOG_ERROR("[QSV] CreateTexture2D failed");
        return nullptr;
    }

    // 获取共享句柄
    CComPtr<IDXGIResource> resource;
    hr = texture->QueryInterface(IID_IDXGIResource, reinterpret_cast<void**>(&resource));
    if (FAILED(hr)) {
        LOG_ERROR("[QSV] Failed to get shared resource");
        return nullptr;
    }

    hr = resource->GetSharedHandle(shared_handle);
    if (FAILED(hr)) {
        LOG_ERROR("[QSV] GetSharedHandle failed");
        return nullptr;
    }

    texture->AddRef();
    return texture.Detach();
}

// ============================================================================
// 帧分配器实现
// ============================================================================

mfxStatus QSVDeviceManager::alloc_internal(
    mfxFrameAllocRequest* request,
    mfxFrameAllocResponse* response) {

    if (!d3d_device_ || !request || !response) {
        return MFX_ERR_NULL_PTR;
    }

    // 确定格式
    DXGI_FORMAT format;
    switch (request->Info.FourCC) {
        case MFX_FOURCC_NV12:
            format = DXGI_FORMAT_NV12;
            break;
        case MFX_FOURCC_P010:
            format = DXGI_FORMAT_P010;
            break;
        case MFX_FOURCC_RGB4:
            format = DXGI_FORMAT_B8G8R8A8_UNORM;
            break;
        case MFX_FOURCC_YUY2:
            format = DXGI_FORMAT_YUY2;
            break;
        default:
            LOG_ERROR("[QSV] Unsupported FourCC: " +
                     std::to_string(request->Info.FourCC));
            return MFX_ERR_UNSUPPORTED;
    }

    // 分配 MemId 数组
    std::vector<QSVMemId*> mids(request->NumFrameSuggested);
    for (size_t i = 0; i < mids.size(); i++) {
        mids[i] = new QSVMemId();
        mids[i]->device = d3d_device_;
        mids[i]->device->AddRef();
        mids[i]->rw = request->Type & 0xF000;  // 保存读写标志
    }

    request->Type = request->Type & 0x0FFF;  // 清除读写标志

    // 创建主纹理
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = request->Info.Width;
    desc.Height = request->Info.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DECODER;
    desc.MiscFlags = 0;

    for (size_t i = 0; i < mids.size(); i++) {
        HRESULT hr = d3d_device_->CreateTexture2D(&desc, nullptr,
                                                  &mids[i]->surface);
        if (FAILED(hr)) {
            LOG_ERROR("[QSV] CreateTexture2D failed for surface " + std::to_string(i));
            for (size_t j = 0; j <= i; j++) {
                delete mids[j];
            }
            return MFX_ERR_MEMORY_ALLOC;
        }
    }

    // 创建 staging 纹理（用于 CPU 访问）
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    for (size_t i = 0; i < mids.size(); i++) {
        HRESULT hr = d3d_device_->CreateTexture2D(&desc, nullptr,
                                                  &mids[i]->stage);
        if (FAILED(hr)) {
            LOG_WARNING("[QSV] CreateTexture2D failed for staging");
            mids[i]->stage = nullptr;  // staging 可能不是必须的
        }
    }

    // 返回 MemId 指针数组
    response->mids = reinterpret_cast<mfxMemId*>(mids.data());
    response->NumFrameActual = static_cast<mfxU16>(mids.size());

    // 保存响应以便后续释放
    std::vector<mfxMemId*> mids_vec(mids.begin(), mids.end());
    response->mids = reinterpret_cast<mfxMemId*>(
        new std::vector<mfxMemId*>(mids_vec));

    return MFX_ERR_NONE;
}

mfxStatus QSVDeviceManager::free_internal(mfxFrameAllocResponse* response) {
    if (!response || !response->mids) {
        return MFX_ERR_NULL_PTR;
    }

    auto* mids_vec = reinterpret_cast<std::vector<mfxMemId*>*>(response->mids);
    if (!mids_vec) {
        return MFX_ERR_NULL_PTR;
    }

    for (auto* mid : *mids_vec) {
        if (mid) {
            delete mid;  // QSVMemId 析构会释放 surface 和 stage
        }
    }

    delete mids_vec;
    response->mids = nullptr;

    return MFX_ERR_NONE;
}

mfxStatus QSVDeviceManager::alloc_frames(
    mfxFrameAllocRequest* request,
    mfxFrameAllocResponse* response) {

    // 对于编码，直接使用内部实现
    return alloc_internal(request, response);
}

mfxStatus QSVDeviceManager::free_frames(mfxFrameAllocResponse* response) {
    return free_internal(response);
}

mfxStatus QSVDeviceManager::lock_frame(mfxMemId mid, mfxFrameData* ptr) {
    if (!d3d_context_ || !mid || !ptr) {
        return MFX_ERR_NULL_PTR;
    }

    auto* mem_id = reinterpret_cast<QSVMemId*>(mid);
    ID3D11Texture2D* surface = mem_id->surface;
    ID3D11Texture2D* stage = mem_id->stage;

    D3D11_TEXTURE2D_DESC desc = {};
    D3D11_MAPPED_SUBRESOURCE locked_rect = {};

    D3D11_MAP map_type = D3D11_MAP_READ;
    UINT map_flags = D3D11_MAP_FLAG_DO_NOT_WAIT;

    if (!stage) {
        // 直接映射
        HRESULT hr = d3d_context_->Map(surface, 0, map_type, map_flags, &locked_rect);
        if (FAILED(hr)) {
            return MFX_ERR_LOCK_MEMORY;
        }
    } else {
        surface->GetDesc(&desc);

        // 如果需要读，复制到 staging
        if (mem_id->rw & WILL_READ) {
            D3D11_BOX box = {0, 0, 0, desc.Width, desc.Height, 1};
            d3d_context_->CopySubresourceRegion(stage, 0, 0, 0, 0, surface, 0, &box);
        }

        HRESULT hr;
        do {
            hr = d3d_context_->Map(stage, 0, map_type, map_flags, &locked_rect);
            if (hr != S_OK && hr != DXGI_ERROR_WAS_STILL_DRAWING) {
                return MFX_ERR_LOCK_MEMORY;
            }
        } while (hr == DXGI_ERROR_WAS_STILL_DRAWING);
    }

    surface->GetDesc(&desc);

    // 根据格式填充指针
    switch (desc.Format) {
        case DXGI_FORMAT_NV12:
            ptr->Pitch = static_cast<mfxU16>(locked_rect.RowPitch);
            ptr->Y = static_cast<mfxU8*>(locked_rect.pData);
            ptr->U = static_cast<mfxU8*>(locked_rect.pData) +
                     desc.Height * locked_rect.RowPitch;
            ptr->V = ptr->U + 1;
            break;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            ptr->Pitch = static_cast<mfxU16>(locked_rect.RowPitch);
            ptr->B = static_cast<mfxU8*>(locked_rect.pData);
            ptr->G = ptr->B + 1;
            ptr->R = ptr->B + 2;
            ptr->A = ptr->B + 3;
            break;
        case DXGI_FORMAT_P010:
            ptr->Pitch = static_cast<mfxU16>(locked_rect.RowPitch);
            ptr->Y = static_cast<mfxU8*>(locked_rect.pData);
            ptr->U = static_cast<mfxU8*>(locked_rect.pData) +
                     desc.Height * locked_rect.RowPitch;
            ptr->V = ptr->U + 2;
            break;
        default:
            if (stage) {
                d3d_context_->Unmap(stage, 0);
            } else {
                d3d_context_->Unmap(surface, 0);
            }
            return MFX_ERR_LOCK_MEMORY;
    }

    return MFX_ERR_NONE;
}

mfxStatus QSVDeviceManager::unlock_frame(mfxMemId mid, mfxFrameData* ptr) {
    if (!d3d_context_ || !mid) {
        return MFX_ERR_NULL_PTR;
    }

    auto* mem_id = reinterpret_cast<QSVMemId*>(mid);
    ID3D11Texture2D* surface = mem_id->surface;
    ID3D11Texture2D* stage = mem_id->stage;

    if (!stage) {
        d3d_context_->Unmap(surface, 0);
    } else {
        d3d_context_->Unmap(stage, 0);

        // 如果需要写，复制回主纹理
        if (mem_id->rw & WILL_WRITE) {
            D3D11_BOX box = {0, 0, 0, 1, 1, 1};  // 需要获取正确尺寸
            d3d_context_->CopySubresourceRegion(surface, 0, 0, 0, 0, stage, 0, nullptr);
        }
    }

    if (ptr) {
        ptr->Pitch = 0;
        ptr->Y = ptr->U = ptr->V = nullptr;
        ptr->R = ptr->G = ptr->B = ptr->A = nullptr;
    }

    return MFX_ERR_NONE;
}

mfxStatus QSVDeviceManager::get_hdl(mfxMemId mid, mfxHDL* handle) {
    if (!handle || !mid) {
        return MFX_ERR_INVALID_HANDLE;
    }

    auto* mem_id = reinterpret_cast<QSVMemId*>(mid);
    auto* pair = new std::pair<mfxHDL, mfxHDL>();
    pair->first = mem_id->surface;  // 主表面纹理
    pair->second = nullptr;
    *handle = pair;

    return MFX_ERR_NONE;
}

mfxStatus QSVDeviceManager::copy_texture(
    QSVMemId* dst, EncoderTexture* src_texture,
    mfxU64 lock_key, mfxU64* next_key) {

    if (!d3d_device_ || !d3d_context_ || !dst || !src_texture) {
        return MFX_ERR_INVALID_HANDLE;
    }

    ID3D11Texture2D* dst_surface = dst->surface;

    // 从共享句柄打开源纹理
    CComPtr<ID3D11Texture2D> input_tex;
    HRESULT hr = d3d_device_->OpenSharedResource(
        src_texture->handle,
        IID_ID3D11Texture2D,
        reinterpret_cast<void**>(&input_tex));

    if (FAILED(hr)) {
        LOG_ERROR("[QSV] OpenSharedResource failed");
        return MFX_ERR_INVALID_HANDLE;
    }

    // 获取 KeyedMutex
    CComPtr<IDXGIKeyedMutex> keyed_mutex;
    hr = input_tex->QueryInterface(IID_IDXGIKeyedMutex,
                                   reinterpret_cast<void**>(&keyed_mutex));
    if (FAILED(hr)) {
        LOG_ERROR("[QSV] QueryInterface IDXGIKeyedMutex failed");
        return MFX_ERR_INVALID_HANDLE;
    }

    input_tex->SetEvictionPriority(DXGI_RESOURCE_PRIORITY_MAXIMUM);

    // 等待生产者释放锁
    hr = keyed_mutex->AcquireSync(lock_key, INFINITE);
    if (FAILED(hr)) {
        LOG_ERROR("[QSV] AcquireSync failed");
        return MFX_ERR_INVALID_HANDLE;
    }

    // 执行 GPU 复制
    D3D11_TEXTURE2D_DESC desc = {};
    input_tex->GetDesc(&desc);
    D3D11_BOX src_box = {0, 0, 0, desc.Width, desc.Height, 1};
    d3d_context_->CopySubresourceRegion(dst_surface, 0, 0, 0, 0,
                                        input_tex, 0, &src_box);

    // 释放锁
    keyed_mutex->ReleaseSync(*next_key);

    return MFX_ERR_NONE;
}

// ============================================================================
// 静态回调实现
// ============================================================================

mfxStatus QSVDeviceManager::static_alloc(
    mfxHDL pthis, mfxFrameAllocRequest* request,
    mfxFrameAllocResponse* response) {

    auto* self = reinterpret_cast<QSVDeviceManager*>(pthis);
    return self->alloc_frames(request, response);
}

mfxStatus QSVDeviceManager::static_free(
    mfxHDL pthis, mfxFrameAllocResponse* response) {

    auto* self = reinterpret_cast<QSVDeviceManager*>(pthis);
    return self->free_frames(response);
}

mfxStatus QSVDeviceManager::static_lock(
    mfxHDL pthis, mfxMemId mid, mfxFrameData* ptr) {

    auto* self = reinterpret_cast<QSVDeviceManager*>(pthis);
    return self->lock_frame(mid, ptr);
}

mfxStatus QSVDeviceManager::static_unlock(
    mfxHDL pthis, mfxMemId mid, mfxFrameData* ptr) {

    auto* self = reinterpret_cast<QSVDeviceManager*>(pthis);
    return self->unlock_frame(mid, ptr);
}

mfxStatus QSVDeviceManager::static_get_hdl(
    mfxHDL pthis, mfxMemId mid, mfxHDL* handle) {

    auto* self = reinterpret_cast<QSVDeviceManager*>(pthis);
    return self->get_hdl(mid, handle);
}

// ============================================================================
// 辅助函数
// ============================================================================

qsv_cpu_platform get_cpu_platform() {
    int regs[4] = {0};
    util_cpuid(regs, 1);

    int family = ((regs[0] >> 8) & 0xF);
    int model = ((regs[0] >> 4) & 0xF);
    int stepping = (regs[0] & 0xF);

    // Extended model
    if (family == 6) {
        int ext_model = ((regs[0] >> 16) & 0xF);
        model |= (ext_model << 4);
    }

    // Simplified detection for Intel
    int cpu_family = family;
    int cpu_model = model;

    // Ivy Bridge and later
    if (cpu_model >= 0x3A) return QSV_CPU_PLATFORM_IVB;
    // Sandy Bridge
    if (cpu_model >= 0x25) return QSV_CPU_PLATFORM_SNB;
    // Westmere
    if (cpu_model >= 0x1F) return QSV_CPU_PLATFORM_WSM;

    return QSV_CPU_PLATFORM_UNKNOWN;
}

bool prefer_igpu(int* i_gpu_index) {
    // 检测是否应该优先使用集显
    // 对于笔记本电脑等场景可能需要
    // 目前简单返回 true（优先 Intel iGPU）
    if (i_gpu_index) {
        *i_gpu_index = 0;
    }
    return true;
}

} // namespace live_assistant
