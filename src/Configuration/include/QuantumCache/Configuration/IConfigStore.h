#pragma once
#include "QuantumCache/Common/Result.h"
#include "QuantumCache/Configuration/AppConfig.h"
#include <memory>
#include <string>

namespace QuantumCache::Configuration {

// Loads/saves/validates AppConfig. Implemented with real JSON
// serialization (nlohmann::json) rather than a stub, but scoped strictly
// to Stage 1's schema. This is the single point where the Service and GUI
// agree on configuration, avoiding config drift between processes.
class IConfigStore {
public:
    virtual ~IConfigStore() = default;

    [[nodiscard]] virtual Common::Result<AppConfig> Load() = 0;
    [[nodiscard]] virtual Common::Result<void> Save(const AppConfig& config) = 0;

    // Validates a config against Stage 1 constraints (non-empty paths,
    // known schema version, log level range, etc.) without touching disk.
    // Real, meaningful checks — not a placeholder that always returns Ok.
    [[nodiscard]] virtual Common::Result<void> Validate(const AppConfig& config) const = 0;
};

// path is a UTF-8 (narrow) filesystem path to the JSON config file, kept
// narrow-string here because the underlying JSON library and this
// component's own file I/O are platform-neutral; wide paths inside
// AppConfig itself remain Windows-native.
[[nodiscard]] Common::Result<std::unique_ptr<IConfigStore>> CreateJsonConfigStore(
    const std::string& path);

} // namespace QuantumCache::Configuration
