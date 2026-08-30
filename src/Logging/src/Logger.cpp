#include "QuantumCache/Logging/Logger.h"

namespace QuantumCache::Logging {

Logger::Logger(LogLevel minimumLevel) : minimumLevel_(minimumLevel) {}

void Logger::AddSink(std::shared_ptr<ILogSink> sink) {
    if (sink) sinks_.push_back(std::move(sink));
}

void Logger::Log(LogLevel level, std::string_view component, std::string_view message) {
    if (static_cast<int>(level) < static_cast<int>(minimumLevel_)) {
        return;
    }
    for (auto& sink : sinks_) {
        sink->Write(level, component, message);
    }
}

void Logger::SetMinimumLevel(LogLevel level) { minimumLevel_ = level; }

LogLevel Logger::MinimumLevel() const noexcept { return minimumLevel_; }

} // namespace QuantumCache::Logging
