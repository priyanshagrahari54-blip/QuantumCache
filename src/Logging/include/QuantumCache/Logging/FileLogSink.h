#pragma once
#include "QuantumCache/Logging/ILogger.h"
#include "QuantumCache/Common/Result.h"
#include <memory>
#include <string>

namespace QuantumCache::Logging {

// Real, working plain-text file sink (portable std::ofstream — this
// component has no Win32-only requirement, unlike Storage/PowerResilience,
// so it is written once and used identically on every platform, including
// this Linux sandbox where its unit tests actually run).
[[nodiscard]] Common::Result<std::unique_ptr<ILogSink>> CreateFileLogSink(const std::string& path);

} // namespace QuantumCache::Logging
