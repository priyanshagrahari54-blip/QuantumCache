#pragma once
#include <cstdint>
#include <string>

namespace QuantumCache::Configuration {

// Configuration schema. Stage 1 contained only the fields needed for the
// components that existed then (storage paths for the session
// marker/journal, logging level, IPC endpoint name). Stage 2 adds the
// settings the real cache engine genuinely needs (capacity, eviction
// policy, write/flush policy, cache enable/disable, and the backing-store
// data file path) — nothing speculative for functionality that still
// doesn't exist (no SSD-tier settings, no predictive-cache settings).
struct AppConfig {
    // Schema version, so future stages can detect and migrate older
    // on-disk configs instead of silently misreading them. Bumped to 2
    // for Stage 2's new fields; JsonConfigStore accepts both 1 (Stage 1
    // configs, defaulted forward) and 2.
    std::uint32_t schemaVersion{2};

    // Directory where power-resilience artifacts (session marker, journal)
    // are stored. Must be on a fixed, writable volume.
    std::wstring stateDirectory{L"C:\\ProgramData\\QuantumCache\\State"};

    // Directory for diagnostic log files.
    std::wstring logDirectory{L"C:\\ProgramData\\QuantumCache\\Logs"};

    // Minimum log level, as an integer matching Logging::LogLevel values
    // (kept as int here to avoid Configuration depending on Logging).
    int minimumLogLevel{2}; // Logging::LogLevel::Info

    // Name of the named-pipe IPC endpoint the Windows Service exposes to
    // the GUI. Kept as plain data; IPC component owns the actual pipe
    // security descriptor logic.
    std::wstring ipcPipeName{L"\\\\.\\pipe\\QuantumCacheControl"};

    // ---------------------------------------------------------------
    // Stage 2: cache engine configuration.
    // ---------------------------------------------------------------

    // Master on/off switch. When false, CacheEngine::Get/Put reject calls
    // with ErrorCode::CacheDisabled rather than silently no-op'ing, so a
    // disabled cache is never mistaken for a working (if empty) one.
    bool cacheEnabled{true};

    // File the FileBackingStore key/value log lives in. Separate from
    // stateDirectory (which holds the session marker + write-ahead
    // journal) because backing-store DATA and power-resilience METADATA
    // are deliberately different files with different lifecycles (the
    // journal is truncated once its contents are durably flushed; the
    // backing store is not).
    std::wstring backingStoreDataFile{L"C:\\ProgramData\\QuantumCache\\State\\backingstore.data"};

    // Total approximate memory budget across all cache shards, in bytes.
    // Mirrors CoreEngine::CacheEngineOptions::capacityBytes (kept as a
    // plain uint64_t here rather than depending on CoreEngine's enum
    // types, matching this file's existing "Configuration does not
    // depend on the components it configures" convention for
    // minimumLogLevel/Logging::LogLevel).
    std::uint64_t cacheCapacityBytes{256ull * 1024 * 1024};

    // Hard cap on total cached entry count. See
    // CoreEngine::CacheEngineOptions::maxEntryCount for why this exists
    // independently of cacheCapacityBytes.
    std::uint64_t cacheMaxEntryCount{100000};

    // Number of independent lock-protected shards. Must be a power of two
    // (validated). Mirrors CoreEngine::CacheEngineOptions::shardCount.
    std::uint32_t cacheShardCount{16};

    // "LeastRecentlyUsed" is the only value Stage 2 implements and
    // accepts; kept as a string (rather than a bare bool/int) so
    // Validate() has a real, extensible, self-describing value to check
    // against as more policies are added in later stages, and so an
    // unimplemented policy name is REJECTED rather than silently
    // treated as LRU.
    std::string evictionPolicy{"LeastRecentlyUsed"};

    // "WriteThrough" or "WriteBackDeferred" — mirrors
    // CoreEngine::WritePolicyKind. See CacheTypes.h for the durability
    // tradeoff each implies.
    std::string writePolicy{"WriteBackDeferred"};

    // "Manual" or "PeriodicBackground" — mirrors
    // CoreEngine::FlushPolicyKind.
    std::string flushPolicy{"Manual"};

    // Only meaningful when flushPolicy == "PeriodicBackground". Must be
    // >= 1 second when that policy is selected (validated).
    std::uint32_t flushIntervalSeconds{30};

    [[nodiscard]] bool operator==(const AppConfig&) const = default;
};

} // namespace QuantumCache::Configuration
