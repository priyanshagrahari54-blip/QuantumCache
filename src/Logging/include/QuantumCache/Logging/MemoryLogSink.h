#pragma once
#include "QuantumCache/Logging/ILogger.h"
#include <mutex>
#include <string>
#include <vector>

namespace QuantumCache::Logging {

// In-memory sink used by unit tests to assert on emitted log records
// without touching the filesystem.
class MemoryLogSink final : public ILogSink {
public:
    struct Entry {
        LogLevel level;
        std::string component;
        std::string message;
    };

    void Write(LogLevel level, std::string_view component, std::string_view message) override {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back(Entry{level, std::string(component), std::string(message)});
    }

    void Flush() override {}

    [[nodiscard]] std::vector<Entry> Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
};

} // namespace QuantumCache::Logging
