#pragma once
#include <cstdint>
#include <string>

namespace QuantumCache::Common {

// Stage 1 error taxonomy. Kept small and honest: codes that are not yet
// backed by real behavior are documented as such rather than silently
// returning "success".
enum class ErrorCode : std::uint32_t {
    Ok = 0,

    Unknown = 1,
    InvalidArgument = 2,
    NotFound = 3,
    AlreadyExists = 4,
    PermissionDenied = 5,
    IoError = 6,
    Timeout = 7,
    CorruptData = 8,
    VersionMismatch = 9,

    // Explicitly for functionality intentionally deferred past Stage 1.
    // Any code path returning this MUST NOT pretend to have done the work.
    NotImplementedStage1 = 100,

    // Power resilience / crash-recovery specific.
    UncleanShutdownDetected = 200,
    RecoveryFailed = 201,
    JournalCorrupt = 202,

    // Platform / environment specific.
    PlatformUnsupported = 300,
    Win32ApiFailure = 301,

    // Stage 2: cache engine specific. Additive block; existing values above
    // are never renumbered so nothing that already depends on them breaks.
    CapacityExceeded = 400,       // Cache is full and eviction could not free enough room.
    RecoveryNotComplete = 401,    // Cache engine used before recovery finished (or after it failed).
    CacheDisabled = 402,          // Operation rejected because AppConfig.cacheEnabled == false.
    ServiceStopping = 403,        // Write rejected because Shutdown() is draining the engine.
    UnflushedDirtyData = 404,     // Invalidate() refused because the entry has unflushed writes.
};

[[nodiscard]] inline const char* ToString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok: return "Ok";
        case ErrorCode::Unknown: return "Unknown";
        case ErrorCode::InvalidArgument: return "InvalidArgument";
        case ErrorCode::NotFound: return "NotFound";
        case ErrorCode::AlreadyExists: return "AlreadyExists";
        case ErrorCode::PermissionDenied: return "PermissionDenied";
        case ErrorCode::IoError: return "IoError";
        case ErrorCode::Timeout: return "Timeout";
        case ErrorCode::CorruptData: return "CorruptData";
        case ErrorCode::VersionMismatch: return "VersionMismatch";
        case ErrorCode::NotImplementedStage1: return "NotImplementedStage1";
        case ErrorCode::UncleanShutdownDetected: return "UncleanShutdownDetected";
        case ErrorCode::RecoveryFailed: return "RecoveryFailed";
        case ErrorCode::JournalCorrupt: return "JournalCorrupt";
        case ErrorCode::PlatformUnsupported: return "PlatformUnsupported";
        case ErrorCode::Win32ApiFailure: return "Win32ApiFailure";
        case ErrorCode::CapacityExceeded: return "CapacityExceeded";
        case ErrorCode::RecoveryNotComplete: return "RecoveryNotComplete";
        case ErrorCode::CacheDisabled: return "CacheDisabled";
        case ErrorCode::ServiceStopping: return "ServiceStopping";
        case ErrorCode::UnflushedDirtyData: return "UnflushedDirtyData";
    }
    return "InvalidErrorCode";
}

struct Error {
    ErrorCode code{ErrorCode::Ok};
    std::string message;
    // Set only when code == Win32ApiFailure and a real GetLastError() value
    // was captured on Windows. Zero/unused on non-Windows builds.
    std::uint32_t platformErrorValue{0};

    [[nodiscard]] bool IsOk() const noexcept { return code == ErrorCode::Ok; }
};

} // namespace QuantumCache::Common
