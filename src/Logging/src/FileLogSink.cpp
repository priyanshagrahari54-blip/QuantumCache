#include "QuantumCache/Logging/FileLogSink.h"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace QuantumCache::Logging {
namespace {

using Common::Error;
using Common::ErrorCode;
using Common::Result;

std::string TimestampNow() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;

    std::tm tmBuf{};
#if defined(_WIN32)
    gmtime_s(&tmBuf, &time);
#else
    gmtime_r(&time, &tmBuf);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tmBuf, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return oss.str();
}

class FileLogSink final : public ILogSink {
public:
    explicit FileLogSink(std::ofstream stream) : stream_(std::move(stream)) {}

    void Write(LogLevel level, std::string_view component, std::string_view message) override {
        std::lock_guard<std::mutex> lock(mutex_);
        stream_ << '[' << TimestampNow() << "] "
                << '[' << ToString(level) << "] "
                << '[' << component << "] "
                << message << '\n';
    }

    void Flush() override {
        std::lock_guard<std::mutex> lock(mutex_);
        stream_.flush();
    }

private:
    std::ofstream stream_;
    std::mutex mutex_;
};

} // namespace

Result<std::unique_ptr<ILogSink>> CreateFileLogSink(const std::string& path) {
    auto stream = std::ofstream(path, std::ios::app | std::ios::out);
    if (!stream.is_open()) {
        return Result<std::unique_ptr<ILogSink>>::Failure(
            Error{ErrorCode::IoError, "failed to open log file: " + path, 0});
    }
    return Result<std::unique_ptr<ILogSink>>::Success(
        std::make_unique<FileLogSink>(std::move(stream)));
}

} // namespace QuantumCache::Logging
