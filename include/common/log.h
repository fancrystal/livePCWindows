#pragma once

#include <string>
#include <iostream>

namespace live_assistant {

enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERR
};

class Log {
public:
    static void debug(const std::string& message, const char* function = nullptr, const char* file = nullptr, int line = 0);
    static void info(const std::string& message, const char* function = nullptr, const char* file = nullptr, int line = 0);
    static void warning(const std::string& message, const char* function = nullptr, const char* file = nullptr, int line = 0);
    static void warn(const std::string& message, const char* function = nullptr, const char* file = nullptr, int line = 0) { warning(message, function, file, line); }
    static void error(const std::string& message, const char* function = nullptr, const char* file = nullptr, int line = 0);

    static void set_level(LogLevel level);

    // 获取当前日志文件路径
    static std::string get_log_file_path();

private:
    static LogLevel current_level_;
    static std::string log_file_path_;  // 当前日志文件路径

    static void log(LogLevel level, const std::string& message, const char* function, const char* file, int line);
    static std::string level_to_string(LogLevel level);
    static std::string generate_log_filename();  // 生成带时间戳的日志文件名
};

// Macro definitions for easy logging
#define LOG_DEBUG(msg) live_assistant::Log::debug(msg, __func__, __FILE__, __LINE__)
#define LOG_INFO(msg) live_assistant::Log::info(msg, __func__, __FILE__, __LINE__)
#define LOG_WARNING(msg) live_assistant::Log::warning(msg, __func__, __FILE__, __LINE__)
#define LOG_ERROR(msg) live_assistant::Log::error(msg, __func__, __FILE__, __LINE__)

} // namespace live_assistant
