#pragma once
#include "QuantumCache/Common/Result.h"
#include "QuantumCache/CoreEngine/CacheTypes.h"
#include "QuantumCache/PowerResilience/RecoveryState.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace QuantumCache::Storage { class IBackingStore; }
namespace QuantumCache::PowerResilience { class IWriteAheadJournal; }
namespace QuantumCache::Logging { class ILogger; }

namespace QuantumCache::CoreEngine {

// -----------------------------------------------------------------------
// STAGE 2 STATUS: REAL, WORKING CACHE ENGINE.
// -----------------------------------------------------------------------
// This is a genuine in-memory read/write cache in front of a real,
// file-backed (Storage::IBackingStore) backing store, with real LRU
// eviction, real capacity accounting, real dirty-state tracking, and real
// crash-recovery integration through Stage 1's IWriteAheadJournal and
// IRecoveryManager. See docs/STAGE2_ARCHITECTURE.md for the full design.
//
// What it explicitly is NOT (see project Stage 2 scope boundaries):
//   - not an SSD/L2 cache — the backing store is a plain file, not a real
//     disk-tiering layer;
//   - not a kernel-mode driver — everything here is user-mode code calling
//     Win32 file APIs (or the portable test double) through IFile;
//   - not predictive/adaptive — eviction is plain LRU, nothing more;
//   - not benchmarked against any third-party product.
//
// Recovery contract (unchanged in spirit from Stage 1, now load-bearing):
// an ICacheEngine instance is constructed in a NOT-READY state. The
// owner (see Service/src/main_service.cpp) MUST call ReplayFromJournal()
// from inside IRecoveryManager::InitializeAndRecover()'s replay callback
// (only invoked when recovery is actually needed), and MUST call
// MarkRecoveryComplete() only after InitializeAndRecover() itself returns
// success. Every data-plane method (Get/Put/Invalidate/Flush/FlushAll)
// rejects calls with ErrorCode::RecoveryNotComplete until
// MarkRecoveryComplete() has been called — there is no way to bypass
// this from outside the class.
class ICacheEngine {
public:
    virtual ~ICacheEngine() = default;

    // Must only be called after the associated IRecoveryManager reports
    // RecoveryState::RecoveryComplete. Implementations must reject calls
    // otherwise (ErrorCode::UncleanShutdownDetected or RecoveryFailed as
    // appropriate) rather than silently operating on a possibly
    // inconsistent store.
    [[nodiscard]] virtual PowerResilience::RecoveryState RequiredRecoveryState() const noexcept {
        return PowerResilience::RecoveryState::RecoveryComplete;
    }

    // -------------------------------------------------------------
    // Recovery-time methods (called by the owner, not by clients).
    // -------------------------------------------------------------

    // Replays every valid cache-semantic record from the write-ahead
    // journal supplied at construction, reconstructing dirty-entry state
    // exactly as it was before the crash/power loss. Idempotent: safe to
    // call more than once (a second call re-scans the same journal
    // content and arrives at the same state) though the normal contract
    // is to call it exactly once, from the IRecoveryManager replay
    // callback. Must be called (or explicitly skipped for a clean
    // shutdown — see main_service.cpp) before MarkRecoveryComplete().
    [[nodiscard]] virtual Common::Result<void> ReplayFromJournal() = 0;

    // Transitions the engine from NOT-READY to READY. Must only be called
    // after the owning IRecoveryManager::InitializeAndRecover() returned
    // success. After this call, Get/Put/Invalidate/Flush/FlushAll become
    // available (subject to CacheEngineOptions::enabled and Shutdown()
    // state).
    [[nodiscard]] virtual Common::Result<void> MarkRecoveryComplete() = 0;

    // -------------------------------------------------------------
    // Read path.
    // -------------------------------------------------------------

    // request -> cache lookup -> hit returns cached bytes; miss -> reads
    // the backing store -> validates the result -> inserts into cache as
    // a clean entry (best-effort; a failure to cache does not fail the
    // read) -> returns the data. Returns ErrorCode::NotFound if the key
    // exists in neither the cache nor the backing store.
    [[nodiscard]] virtual Common::Result<std::vector<std::uint8_t>> Get(
        const std::string& key) = 0;

    [[nodiscard]] virtual bool Contains(const std::string& key) = 0;

    [[nodiscard]] virtual Common::Result<CacheEntryInfo> GetEntryInfo(
        const std::string& key) const = 0;

    // -------------------------------------------------------------
    // Write path.
    // -------------------------------------------------------------

    // Inserts or updates `key`. Durability boundary depends on
    // CacheEngineOptions::writePolicy:
    //   WriteBackDeferred (default): returns success once the write is
    //     durably JOURNALED (survives crash/power loss) — NOT once it is
    //     in the backing store. The entry is Dirty until a later Flush.
    //   WriteThrough: returns success only once the write is durably
    //     journaled AND confirmed written to the backing store, at which
    //     point the entry is Clean.
    // Never returns success before the applicable boundary above has
    // actually been reached.
    [[nodiscard]] virtual Common::Result<void> Put(
        const std::string& key, const std::vector<std::uint8_t>& value) = 0;

    // Removes a CLEAN entry from the cache (both in-memory and via a
    // journaled Invalidate record for audit/replay purposes). Refuses
    // with ErrorCode::UnflushedDirtyData if the entry is Dirty or
    // FlushInProgress — invalidating unflushed data would silently
    // discard a write that has not yet reached the backing store, which
    // this project's safety rules forbid by default. Use
    // ForceInvalidate() to do that explicitly.
    [[nodiscard]] virtual Common::Result<void> Invalidate(const std::string& key) = 0;

    // Like Invalidate(), but permitted regardless of dirty state. Still
    // journals the removal (durably) before applying it in memory, so a
    // crash between the two leaves recovery able to observe and apply the
    // same removal. This is the ONLY method in the engine that may
    // discard dirty data, and it only ever does so because the caller
    // explicitly asked for exactly that.
    [[nodiscard]] virtual Common::Result<void> ForceInvalidate(const std::string& key) = 0;

    // -------------------------------------------------------------
    // Deferred-write / flush path.
    // -------------------------------------------------------------

    // Flushes one dirty entry to the backing store: journals FlushIntent
    // (durable), writes the backing store, then journals FlushComplete
    // (durable) and marks the entry Clean — but only if no newer Put()
    // superseded it while the flush was in flight (in which case the
    // newer write's own dirty state is left untouched rather than
    // incorrectly marked clean).
    [[nodiscard]] virtual Common::Result<void> Flush(const std::string& key) = 0;

    // Flushes every currently dirty entry. Best-effort: continues past
    // individual per-key failures so one bad entry cannot block flushing
    // the rest, but the overall Result reports failure (with the most
    // recent per-key error) if any entry failed to flush.
    [[nodiscard]] virtual Common::Result<void> FlushAll() = 0;

    // -------------------------------------------------------------
    // Observability.
    // -------------------------------------------------------------

    [[nodiscard]] virtual CacheStatistics GetStatistics() const noexcept = 0;

    // -------------------------------------------------------------
    // Shutdown.
    // -------------------------------------------------------------

    // Stops accepting new Put/Invalidate/ForceInvalidate calls
    // immediately (they return ErrorCode::ServiceStopping), attempts a
    // best-effort FlushAll(), then stops accepting Get() calls too. Safe
    // to call even if some entries fail to flush: unflushed data remains
    // durably journaled and will be recovered/replayed on the next
    // startup, so a flush failure here does not lose data — it only means
    // more work for the next ReplayFromJournal().
    [[nodiscard]] virtual Common::Result<void> Shutdown() = 0;
};

// Constructs a real cache engine instance in the NOT-READY state (see
// class comment above). `backingStore` and `journal` must be non-null;
// `logger` is optional (pass nullptr to disable engine logging).
[[nodiscard]] Common::Result<std::unique_ptr<ICacheEngine>> CreateCacheEngine(
    const CacheEngineOptions& options,
    std::shared_ptr<Storage::IBackingStore> backingStore,
    std::shared_ptr<PowerResilience::IWriteAheadJournal> journal,
    std::shared_ptr<Logging::ILogger> logger = nullptr);

} // namespace QuantumCache::CoreEngine
