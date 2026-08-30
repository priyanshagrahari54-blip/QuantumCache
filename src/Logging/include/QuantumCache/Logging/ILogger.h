#pragma once
#include <string>
#include <string_view>

namespace QuantumCache::Logging {

enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warning = 3,
    Error = 4,
    Critical = 5,
};

[[nodiscard]] inline const char* ToString(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Critical: return "CRITICAL";
    }
    return "UNKNOWN";
}

// Diagnostics logging interface shared by every component (GUI, Service,
// Core Engine, Storage, IPC). Deliberately simple in Stage 1: leveled,
// synchronous, component-tagged text logging to a file, with an
// injectable sink so unit tests can capture output without touching disk.
// Structured/binary tracing (e.g. ETW on real Windows) is explicitly
// deferred, not faked.
class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual void Write(LogLevel level, std::string_view component, std::string_view message) = 0;
    virtual void Flush() = 0;
};

class ILogger {
public:
    virtual ~ILogger() = default;

    virtual void Log(LogLevel level, std::string_view component, std::string_view message) = 0;
    virtual void SetMinimumLevel(LogLevel level) = 0;
    [[nodiscard]] virtual LogLevel MinimumLevel() const noexcept = 0;
};

} // namespace QuantumCache::Logging
