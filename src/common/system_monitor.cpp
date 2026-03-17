#include "common/system_monitor.h"
#include "common/log.h"
#include <sstream>
#include <iomanip>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>

// WRL (Windows Runtime Library) for ComPtr
#include <wrl/client.h>

// DXGI/D3D11 头文件 (用于 GPU 监控)
#include <dxgi1_6.h>
#include <d3d11.h>
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")

// WRL namespace
using Microsoft::WRL::ComPtr;
#endif

namespace live_assistant {

//=============================================================================
// SystemStats 实现
//=============================================================================

std::string SystemStats::to_string() const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);

    oss << "CPU: " << cpu_usage_percent << "% | "
        << "内存: " << memory_usage_percent << "% ("
        << (memory_used_bytes / 1024 / 1024) << " MB / "
        << (memory_total_bytes / 1024 / 1024) << " MB)";

    if (gpu_available) {
        oss << " | GPU: " << gpu_usage_percent << "% ("
            << (gpu_memory_used_bytes / 1024 / 1024) << " MB / "
            << (gpu_memory_total_bytes / 1024 / 1024) << " MB)";
    } else {
        oss << " | GPU: N/A";
    }

    return oss.str();
}

std::string SystemStats::to_short_string() const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    oss << "CPU: " << cpu_usage_percent << "% | "
        << "内存: " << memory_usage_percent << "% ("
        << (memory_used_bytes / 1024 / 1024) << " MB)";
    return oss.str();
}

//=============================================================================
// SystemMonitor 实现
//=============================================================================

SystemMonitor::SystemMonitor() {
    LOG_INFO("SystemMonitor: Initializing...");

#ifdef _WIN32
    init_cpu_monitor();
    init_gpu_monitor();
#endif

    // 初始化时获取一次基准值
    update();

    LOG_INFO("SystemMonitor: Initialization complete");
}

SystemMonitor::~SystemMonitor() {
    // DXGI 资源会自动释放 (ComPtr 智能指针)
    LOG_INFO("SystemMonitor: Destroyed");
}

void SystemMonitor::update() {
    cached_stats_ = get_current_stats();
}

SystemStats SystemMonitor::get_current_stats() {
    SystemStats stats;

    // CPU 使用率
    stats.cpu_usage_percent = get_cpu_usage();

    // 内存使用情况
    auto mem_info = get_memory_info();
    stats.memory_total_bytes = mem_info.total_bytes;
    stats.memory_used_bytes = mem_info.used_bytes;
    stats.memory_available_bytes = mem_info.available_bytes;
    stats.memory_usage_percent = mem_info.usage_percent;

    // GPU 使用情况
    auto gpu_info = get_gpu_info();
    stats.gpu_usage_percent = gpu_info.usage_percent;
    stats.gpu_memory_total_bytes = gpu_info.memory_total_bytes;
    stats.gpu_memory_used_bytes = gpu_info.memory_used_bytes;
    stats.gpu_memory_free_bytes = gpu_info.memory_free_bytes;
    stats.gpu_memory_usage_percent = gpu_info.memory_usage_percent;
    stats.gpu_available = gpu_info.available;

    return stats;
}

double SystemMonitor::get_cpu_usage() {
    double usage = 0.0;

#ifdef _WIN32
    FILETIME idleTime, kernelTime, userTime;
    if (GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
        auto filetime_to_uint64 = [](const FILETIME& ft) -> uint64_t {
            return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        };

        uint64_t idle = filetime_to_uint64(idleTime);
        uint64_t kernel = filetime_to_uint64(kernelTime);
        uint64_t user = filetime_to_uint64(userTime);
        uint64_t sys = kernel + user;

        if (first_cpu_sample_) {
            prev_idle_time_ = idle;
            prev_kernel_time_ = kernel;
            prev_user_time_ = user;
            first_cpu_sample_ = false;
        } else {
            uint64_t idle_delta = idle - prev_idle_time_;
            uint64_t sys_delta = sys - (prev_kernel_time_ + prev_user_time_);

            if (sys_delta > 0) {
                usage = (1.0 - (static_cast<double>(idle_delta) / static_cast<double>(sys_delta))) * 100.0;
                // Clamp to [0, 100] range (avoid Windows macro conflicts)
                if (usage < 0.0) usage = 0.0;
                if (usage > 100.0) usage = 100.0;
            }

            prev_idle_time_ = idle;
            prev_kernel_time_ = kernel;
            prev_user_time_ = user;
        }
    }
#endif

    return usage;
}

SystemMonitor::MemoryInfo SystemMonitor::get_memory_info() {
    MemoryInfo info{};

#ifdef _WIN32
    MEMORYSTATUSEX memx{};
    memx.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memx)) {
        info.total_bytes = memx.ullTotalPhys;
        info.available_bytes = memx.ullAvailPhys;
        info.used_bytes = info.total_bytes - info.available_bytes;
        info.usage_percent = static_cast<double>(memx.dwMemoryLoad);
    }
#endif

    return info;
}

SystemMonitor::GPUInfo SystemMonitor::get_gpu_info() {
    GPUInfo info{};
    info.available = false;

#ifdef _WIN32
    // 使用 DXGI 获取 GPU 信息 (Windows 通用方法)
    if (dxgi_available_) {
        info = get_gpu_info_dxgi();
    }
#endif

    return info;
}

//=============================================================================
// Windows 特定实现
//=============================================================================

#ifdef _WIN32

void SystemMonitor::init_cpu_monitor() {
    LOG_INFO("SystemMonitor: CPU monitor initialized (Windows GetSystemTimes)");
}

void SystemMonitor::init_gpu_monitor() {
    LOG_INFO("SystemMonitor: Initializing GPU monitor...");

    // 尝试初始化 DXGI (Windows 通用 GPU 监控)
    dxgi_available_ = init_dxgi_gpu_monitor();
    if (dxgi_available_) {
        LOG_INFO("SystemMonitor: DXGI GPU monitoring enabled");
    } else {
        LOG_WARNING("SystemMonitor: GPU monitoring not available (DXGI initialization failed)");
    }
}

bool SystemMonitor::init_dxgi_gpu_monitor() {
    // 尝试创建 DXGI 工厂来检测 GPU
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), &factory);
    return SUCCEEDED(hr);
}

SystemMonitor::GPUInfo SystemMonitor::get_gpu_info_dxgi() {
    GPUInfo info{};
    info.available = false;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), &factory))) {
        return info;
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); ++i) {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            // 找到第一个独立的 GPU (NVIDIA or AMD)
            if (desc.VendorId == 0x10DE ||  // NVIDIA
                desc.VendorId == 0x1002) {  // AMD
                // 优先使用 IDXGIAdapter3::QueryVideoMemoryInfo 获取实时显存占用
                ComPtr<IDXGIAdapter3> adapter3;
                if (SUCCEEDED(adapter.As(&adapter3)) && adapter3) {
                    DXGI_QUERY_VIDEO_MEMORY_INFO mem_info{};
                    if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
                            0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mem_info))) {
                        // Budget 更接近“可用总量”，DedicatedVideoMemory 更接近“物理显存”
                        const uint64_t total = (mem_info.Budget > 0) ? mem_info.Budget
                                                                     : static_cast<uint64_t>(desc.DedicatedVideoMemory);
                        const uint64_t used = static_cast<uint64_t>(mem_info.CurrentUsage);
                        info.memory_total_bytes = total;
                        info.memory_used_bytes = used;
                        info.memory_free_bytes = (total > used) ? (total - used) : 0;
                        info.memory_usage_percent = (total > 0)
                            ? (static_cast<double>(used) * 100.0 / static_cast<double>(total))
                            : 0.0;
                        info.available = true;

                        // DXGI 无法提供“GPU核心利用率”，这里用显存占用率填充 usage_percent 以便 UI 显示百分比
                        info.usage_percent = info.memory_usage_percent;
                    }
                }

                // 若 QueryVideoMemoryInfo 失败，退化为仅提供总显存（不再做 50% 估算）
                if (!info.available) {
                    info.memory_total_bytes = static_cast<uint64_t>(desc.DedicatedVideoMemory);
                    info.memory_used_bytes = 0;
                    info.memory_free_bytes = info.memory_total_bytes;
                    info.memory_usage_percent = 0.0;
                    info.usage_percent = 0.0;
                    info.available = (info.memory_total_bytes > 0);
                }

                break;
            }
        }
    }

    return info;
}

#endif // _WIN32

//=============================================================================
// 全局单例
//=============================================================================

SystemMonitor& system_monitor() {
    static SystemMonitor instance;
    return instance;
}

} // namespace live_assistant
