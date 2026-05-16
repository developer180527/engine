#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdarg>

enum class LogLevel { Debug, Info, Success, Warning, Error };

struct LogEntry {
    LogLevel    level;
    std::string tag;      // e.g. "Loader", "Assimp", "ECS", "Render"
    std::string message;
    float       timestamp;
};

class Logger {
public:
    static Logger& get() { static Logger inst; return inst; }

    void log(LogLevel level, const char* tag, const char* fmt, ...) {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        std::printf("[%s] %s\n", tag, buf);

        float ts = 0.0f;
        {
            using namespace std::chrono;
            ts = duration<float>(steady_clock::now() - m_start).count();
        }
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_entries.size() >= 1024)
            m_entries.erase(m_entries.begin());
        m_entries.push_back({level, tag, buf, ts});
    }

    std::vector<LogEntry> snapshot() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_entries;
    }

    void clear() {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_entries.clear();
    }

private:
    Logger() : m_start(std::chrono::steady_clock::now()) {}
    mutable std::mutex            m_mutex;
    std::vector<LogEntry>         m_entries;
    std::chrono::steady_clock::time_point m_start;
};

#define LOG_INFO(tag, ...)    Logger::get().log(LogLevel::Info,    tag, __VA_ARGS__)
#define LOG_SUCCESS(tag, ...) Logger::get().log(LogLevel::Success, tag, __VA_ARGS__)
#define LOG_WARN(tag, ...)    Logger::get().log(LogLevel::Warning, tag, __VA_ARGS__)
#define LOG_ERROR(tag, ...)   Logger::get().log(LogLevel::Error,   tag, __VA_ARGS__)
#define LOG_DEBUG(tag, ...)   Logger::get().log(LogLevel::Debug,   tag, __VA_ARGS__)
