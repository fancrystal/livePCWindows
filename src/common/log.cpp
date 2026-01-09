#include "common/log.h"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <thread>
#include <cstdarg>
#include <fstream>

namespace live_assistant {

LogLevel Log::current_level_ = LogLevel::INFO;

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
    // Also append to a log file in current working directory for easier capture
    try {
        std::ofstream ofs("liveassistant_run.log", std::ios::app);
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

} // namespace live_assistant