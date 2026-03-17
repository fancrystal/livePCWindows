#include "common/log.h"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <thread>
#include <cstdarg>
#include <fstream>
#include <filesystem>

namespace live_assistant {

LogLevel Log::current_level_ = LogLevel::INFO;
std::string Log::log_file_path_;

std::string Log::level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG:
            return "DEBUG";
        case LogLevel::INFO:
            return "INFO";
        case LogLevel::WARN:
            return "WARNING";
        case LogLevel::ERR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

std::string get_current_time() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm local_tm;
    localtime_s(&local_tm, &time_t_now);

    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S")
        << "." << std::setw(3) << std::setfill('0') << ms.count();

    return oss.str();
}

std::string get_thread_id() {
    std::ostringstream oss;
    oss << std::this_thread::get_id();
    return oss.str();
}

std::string Log::generate_log_filename() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);

    std::tm local_tm;
    localtime_s(&local_tm, &time_t_now);

    // 创建 applogs 目录（如果不存在）
    const std::string log_dir = "applogs";
    try {
        if (!std::filesystem::exists(log_dir)) {
            std::filesystem::create_directory(log_dir);
        }
    } catch (...) {
        // 如果创建失败，使用当前目录
    }

    std::ostringstream oss;
    oss << log_dir << "/LiveAssistant_"
        << std::put_time(&local_tm, "%Y%m%d_%H%M%S")
        << ".log";

    return oss.str();
}

void Log::log(LogLevel level, const std::string& message, const char* function, const char* file, int line) {
    if (level < current_level_) {
        return;
    }

    std::ostringstream oss;
    oss << "[" << get_current_time() << "] "
        << "[" << level_to_string(level) << "] "
        << "[" << get_thread_id() << "] ";

    // 处理空指针情况，确保只在指针非空时才访问
    if (file != nullptr && function != nullptr) {
        oss << "[" << file << ":" << line << " " << function << "] ";
    } else if (file != nullptr) {
        oss << "[" << file << ":" << line << "] ";
    } else if (function != nullptr) {
        oss << "[" << function << "] ";
    } else {
        oss << "[" << "unknown" << "] ";
    }

    oss << message << std::endl;

    std::cout << oss.str();

    // 首次调用时生成日志文件名
    if (log_file_path_.empty()) {
        log_file_path_ = generate_log_filename();
        // 在日志文件开头写入启动标记
        try {
            std::ofstream ofs(log_file_path_, std::ios::app);
            if (ofs.is_open()) {
                ofs << "========================================" << std::endl;
                ofs << "LiveAssistant Log - Session Started" << std::endl;
                ofs << "Log file: " << log_file_path_ << std::endl;
                ofs << "========================================" << std::endl;
                ofs.close();
            }
        } catch (...) {
            // ignore file errors
        }
    }

    // 追加日志到文件
    try {
        std::ofstream ofs(log_file_path_, std::ios::app);
        if (ofs.is_open()) {
            ofs << oss.str();
            ofs.close();
        }
    } catch (...) {
        // ignore file errors
    }
}

void Log::debug(const std::string& message, const char* function, const char* file, int line) {
    log(LogLevel::DEBUG, message, function, file, line);
}

void Log::info(const std::string& message, const char* function, const char* file, int line) {
    log(LogLevel::INFO, message, function, file, line);
}

void Log::warning(const std::string& message, const char* function, const char* file, int line) {
    log(LogLevel::WARN, message, function, file, line);
}

void Log::error(const std::string& message, const char* function, const char* file, int line) {
    log(LogLevel::ERR, message, function, file, line);
}

void Log::set_level(LogLevel level) {
    current_level_ = level;
}

std::string Log::get_log_file_path() {
    return log_file_path_;
}

} // namespace live_assistant
