#pragma once
#include "QuantumCache/Common/Result.h"
#include "QuantumCache/PowerResilience/RecoveryState.h"
#include <functional>
#include <memory>

namespace QuantumCache::PowerResilience {

class ISessionMarker;
class IWriteAheadJournal;

// Coordinates ISessionMarker + IWriteAheadJournal to answer, at process
// startup, "did we crash / lose power last time, and if so, what is the
// recovered state?" This is the top-level object the Windows Service and
// GUI both consult before allowing any cache/storage layer above it to be
// used. It does not know about cache semantics; it knows about the
// generic recover-or-quarantine lifecycle.
using JournalReplayHandler = std::function<Common::Result<void>()>;

class IRecoveryManager {
public:
    virtual ~IRecoveryManager() = default;

    // Must be called exactly once at startup, before any other component
    // touches persisted cache state. Internally:
    //  1. Reads the previous session marker.
    //  2. If it indicates a clean shutdown (or no prior session), moves
    //     straight to RecoveryComplete and starts a new session.
    //  3. Otherwise transitions Unknown -> UncleanShutdownDetected ->
    //     RecoveryInProgress, invokes `onReplayNeeded` (which future
    //     components, e.g. the real cache engine, will use to fold
    //     journaled records into their own state), then transitions to
    //     RecoveryComplete or RecoveryFailed depending on the result.
    [[nodiscard]] virtual Common::Result<void> InitializeAndRecover(
        const JournalReplayHandler& onReplayNeeded) = 0;

    [[nodiscard]] virtual RecoveryState CurrentState() const noexcept = 0;

    // Must be called on orderly shutdown paths (normal exit, Windows
    // Service SERVICE_CONTROL_STOP/SHUTDOWN/PRESHUTDOWN handlers). Marks
    // the session as cleanly closed so the NEXT startup does not think a
    // power loss occurred.
    [[nodiscard]] virtual Common::Result<void> MarkCleanShutdown() = 0;
};

// Factory: composes a RecoveryManager from an already-constructed
// ISessionMarker and IWriteAheadJournal (see CreateSessionMarker /
// CreateWriteAheadJournal).
//
// Stage 2 change: `journal` is accepted as a shared_ptr rather than a
// unique_ptr. Stage 1's RecoveryManager never actually called into the
// journal itself (replay logic was entirely caller-supplied via
// `onReplayNeeded`); Stage 2's cache engine needs to keep appending to
// and truncating the SAME physical journal for the rest of the process
// lifetime, so both this RecoveryManager and the cache engine now hold
// shared ownership of one journal instance instead of the journal being
// exclusively owned (and effectively unused) by RecoveryManager alone.
// This is source-compatible with existing call sites that pass
// `std::move(someUniquePtr)`, since std::unique_ptr converts implicitly
// to std::shared_ptr.
[[nodiscard]] Common::Result<std::unique_ptr<IRecoveryManager>> CreateRecoveryManager(
    std::unique_ptr<ISessionMarker> marker,
    std::shared_ptr<IWriteAheadJournal> journal);

} // namespace QuantumCache::PowerResilience
