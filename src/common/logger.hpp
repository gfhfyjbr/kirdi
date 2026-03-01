#pragma once

#include <string>
#include <string_view>
#include <iostream>
#include <mutex>
#include <chrono>
#include <format>
#include <source_location>

namespace kirdi {

enum class LogLevel : uint8_t {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Fatal = 5,
};

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void set_level(LogLevel level) { level_ = level; }
    LogLevel level() const { return level_; }

    void log(LogLevel level, std::string_view msg,
             const std::source_location& loc = std::source_location::current())
    {
        if (level < level_) return;

        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::floor<std::chrono::seconds>(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - time);

        std::lock_guard lock(mutex_);
        std::cerr << std::format("{:%H:%M:%S}.{:03d} {} [{}:{}] {}\n",
            time, ms.count(),
            level_str(level),
            extract_filename(loc.file_name()),
            loc.line(),
            msg
        );
    }

private:
    Logger() = default;
    LogLevel level_ = LogLevel::Info;
    std::mutex mutex_;

    static constexpr std::string_view level_str(LogLevel l) {
        switch (l) {
            case LogLevel::Trace: return "TRACE";
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Info:  return " INFO";
            case LogLevel::Warn:  return " WARN";
            case LogLevel::Error: return "ERROR";
            case LogLevel::Fatal: return "FATAL";
        }
        return "?????";
    }

    static constexpr std::string_view extract_filename(const char* path) {
        std::string_view sv(path);
        auto pos = sv.find_last_of("/\\");
        return pos != std::string_view::npos ? sv.substr(pos + 1) : sv;
    }
};

// ── Convenience macros ──────────────────────────────────────────────────────
#define LOG_TRACE(msg) ::kirdi::Logger::instance().log(::kirdi::LogLevel::Trace, msg)
#define LOG_DEBUG(msg) ::kirdi::Logger::instance().log(::kirdi::LogLevel::Debug, msg)
#define LOG_INFO(msg)  ::kirdi::Logger::instance().log(::kirdi::LogLevel::Info,  msg)
#define LOG_WARN(msg)  ::kirdi::Logger::instance().log(::kirdi::LogLevel::Warn,  msg)
#define LOG_ERROR(msg) ::kirdi::Logger::instance().log(::kirdi::LogLevel::Error, msg)
#define LOG_FATAL(msg) ::kirdi::Logger::instance().log(::kirdi::LogLevel::Fatal, msg)

// Format variants
#define LOG_INFOF(...) ::kirdi::Logger::instance().log(::kirdi::LogLevel::Info,  std::format(__VA_ARGS__))
#define LOG_WARNF(...) ::kirdi::Logger::instance().log(::kirdi::LogLevel::Warn,  std::format(__VA_ARGS__))
#define LOG_ERRORF(...) ::kirdi::Logger::instance().log(::kirdi::LogLevel::Error, std::format(__VA_ARGS__))
#define LOG_DEBUGF(...) ::kirdi::Logger::instance().log(::kirdi::LogLevel::Debug, std::format(__VA_ARGS__))

} // namespace kirdi
