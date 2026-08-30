#pragma once
#include "QuantumCache/Logging/ILogger.h"
#include <memory>
#include <vector>

namespace QuantumCache::Logging {

// Real, working leveled logger that fans out to zero or more ILogSink
// instances (typically one FileLogSink in the service/GUI, plus an
// in-memory capturing sink in unit tests). Thread-safety is delegated to
// each sink implementation.
class Logger final : public ILogger {
public:
    explicit Logger(LogLevel minimumLevel = LogLevel::Info);

    void AddSink(std::shared_ptr<ILogSink> sink);

    void Log(LogLevel level, std::string_view component, std::string_view message) override;
    void SetMinimumLevel(LogLevel level) override;
    [[nodiscard]] LogLevel MinimumLevel() const noexcept override;

private:
    LogLevel minimumLevel_;
    std::vector<std::shared_ptr<ILogSink>> sinks_;
};

} // namespace QuantumCache::Logging
