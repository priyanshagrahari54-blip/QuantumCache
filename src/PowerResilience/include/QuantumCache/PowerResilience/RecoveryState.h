#pragma once
#include <cstdint>

namespace QuantumCache::PowerResilience {

// The lifecycle a QuantumCache session's on-disk state can be in with
// respect to crash/power-loss consistency. This is the model the rest of
// Stage 1 (and later, the real cache/deferred-write engine) is built
// against. Nothing here fakes recovery — Stage 1 only implements enough to
// *detect* an unclean shutdown and *replay* a write-ahead journal that is
// itself empty of real cache semantics until the cache engine exists.
enum class RecoveryState : std::uint32_t {
    // No session marker has ever been observed; first run on this store.
    Unknown = 0,

    // The previous session's marker was cleared in an orderly fashion
    // (OnCleanShutdown() was called and completed) - no recovery needed.
    CleanShutdown = 1,

    // A session marker from a previous run was found still "open" at
    // startup, meaning the process/service/machine did not shut down in
    // an orderly way (e.g. power loss, crash, kill -9, BSOD). Recovery is
    // required before the cache/storage layers may be used.
    UncleanShutdownDetected = 2,

    // RecoveryManager is actively replaying/validating the write-ahead
    // journal and any other crash-consistency artifacts.
    RecoveryInProgress = 3,

    // Journal replay and validation completed and the store is safe to use.
    RecoveryComplete = 4,

    // Journal replay could not bring the store to a consistent state.
    // Stage 1 policy: the affected volume/cache MUST be treated as
    // unusable/quarantined rather than silently continuing.
    RecoveryFailed = 5,
};

[[nodiscard]] inline const char* ToString(RecoveryState state) noexcept {
    switch (state) {
        case RecoveryState::Unknown: return "Unknown";
        case RecoveryState::CleanShutdown: return "CleanShutdown";
        case RecoveryState::UncleanShutdownDetected: return "UncleanShutdownDetected";
        case RecoveryState::RecoveryInProgress: return "RecoveryInProgress";
        case RecoveryState::RecoveryComplete: return "RecoveryComplete";
        case RecoveryState::RecoveryFailed: return "RecoveryFailed";
    }
    return "InvalidRecoveryState";
}

} // namespace QuantumCache::PowerResilience
