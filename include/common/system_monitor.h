#pragma once

#include <string>
#include <cstdint>

namespace live_assistant {

// 系统资源使用情况结构体
struct SystemStats {
    // CPU
    double cpu_usage_percent = 0.0;       // CPU使用率 (0-100)

    // 内存
    uint64_t memory_total_bytes = 0;       // 总内存 (字节)
    uint64_t memory_used_bytes = 0;        // 已用内存 (字节)
    uint64_t memory_available_bytes = 0;   // 可用内存 (字节)
    double memory_usage_percent = 0.0;     // 内存使用率 (0-100)

    // GPU (如果可用)
    double gpu_usage_percent = 0.0;        // GPU使用率 (0-100)
    uint64_t gpu_memory_total_bytes = 0;   // GPU总内存 (字节)
    uint64_t gpu_memory_used_bytes = 0;    // GPU已用内存 (字节)
    uint64_t gpu_memory_free_bytes = 0;    // GPU可用内存 (字节)
    double gpu_memory_usage_percent = 0.0; // GPU内存使用率 (0-100)
    bool gpu_available = false;            // GPU信息是否可用

    // 格式化显示方法
    std::string to_string() const;
    std::string to_short_string() const;   // 简短格式: "CPU: XX% | 内存: XX% (XXX MB)"
};

// 系统监控类
class SystemMonitor {
public:
    SystemMonitor();
    ~SystemMonitor();

    // 获取当前系统资源使用情况
    SystemStats get_current_stats();

    // 获取 CPU 使用率 (0-100)
    double get_cpu_usage();

    // 获取内存使用情况
    struct MemoryInfo {
        uint64_t total_bytes;
        uint64_t used_bytes;
        uint64_t available_bytes;
        double usage_percent;
    };
    MemoryInfo get_memory_info();

    // 获取 GPU 使用情况 (如果可用)
    struct GPUInfo {
        double usage_percent;
        uint64_t memory_total_bytes;
        uint64_t memory_used_bytes;
        uint64_t memory_free_bytes;
        double memory_usage_percent;
        bool available;
    };
    GPUInfo get_gpu_info();

    // 更新缓存值 (用于定期更新)
    void update();

    // 获取缓存的统计值 (避免频繁查询系统)
    const SystemStats& get_cached_stats() const { return cached_stats_; }

private:
    SystemStats cached_stats_;

    // Windows 特定实现
#ifdef _WIN32
    void init_cpu_monitor();
    void init_gpu_monitor();

    // CPU 监控相关
    uint64_t prev_idle_time_ = 0;
    uint64_t prev_kernel_time_ = 0;
    uint64_t prev_user_time_ = 0;
    bool first_cpu_sample_ = true;

    // GPU 监控相关
    bool dxgi_available_ = false;      // Windows DXGI (通用)

    // DXGI 诊断 (用于 Windows 通用 GPU 监控)
    bool init_dxgi_gpu_monitor();
    GPUInfo get_gpu_info_dxgi();
#endif
};

// 全局单例访问
SystemMonitor& system_monitor();

} // namespace live_assistant
