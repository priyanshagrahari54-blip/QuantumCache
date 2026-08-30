// Real, working cache engine implementation. See ICacheEngine.h for the
// full contract and docs/STAGE2_ARCHITECTURE.md for the design rationale
// behind the concurrency model, capacity/eviction strategy, and durability
// state machine summarized here.
//
// ---------------------------------------------------------------------
// CONCURRENCY MODEL (see docs/STAGE2_ARCHITECTURE.md "Concurrency" for
// the full write-up; this is the short version co-located with the code
// it describes):
// ---------------------------------------------------------------------
//   - The keyspace is split across `shardCount` independent Shard objects,
//     each with its own std::mutex guarding only that shard's in-memory
//     map/LRU list/counters. Two operations on keys that hash to
//     different shards never contend.
//   - ALL journal appends go through one journalMutex_, because
//     PowerResilience::IWriteAheadJournal (Stage 1) is not documented or
//     implemented as thread-safe, and because the journal's on-disk order
//     is the actual source of truth for "what happened when" during
//     replay — see "Version assignment" below.
//   - Lock ordering rule: a shard lock is NEVER held while acquiring
//     journalMutex_ or while performing backing-store I/O, and vice
//     versa. Every operation here follows the same three-phase shape:
//       1) brief shard-lock "peek" (read current state),
//       2) unlocked I/O (journal append / backing-store read-write),
//       3) brief shard-lock "commit" (apply the now-durable result,
//          discarding it if a newer write already superseded it).
//     This means no code path here ever holds two locks at once, so lock
//     ordering deadlocks between shard mutexes and journalMutex_ are
//     structurally impossible.
//   - Version assignment: every Upsert/Invalidate is tagged with a value
//     from one engine-wide std::atomic<uint64_t> counter that is
//     incremented ONLY while journalMutex_ is held, immediately before
//     the corresponding Append() call. This guarantees version order ==
//     journal append order for ALL keys, which is what makes the
//     "commit" phase safe: when applying a write to memory, we only
//     apply it if its version is newer than what is currently in the
//     shard, so a slow thread can never clobber a fast thread's
//     already-applied newer write, no matter which thread's OS
//     scheduling makes it to the commit phase first.
//   - Read-fill linearization (audited/fixed — see
//     docs/STAGE2_ARCHITECTURE.md "Read-miss/write race" for the full
//     writeup): a cache-miss read from the backing store (in Get()) is
//     NOT a journaled mutation, and it must NEVER be allowed to look
//     "newer" than a real write to the same key, no matter how the two
//     threads happen to interleave. Two independent mechanisms are
//     required together — neither one alone is sufficient (see the
//     detailed rationale, including the two-step failure analysis, next
//     to Get()'s implementation):
//       1) Per-shard mutation fence (`Shard::mutationFence`): a plain
//          counter, incremented under shard.mutex every time a real
//          commit changes the shard's live state (an insert/update via
//          InsertOrUpdateLocked, or a removal via InvalidateImpl).
//          Get()'s miss path snapshots this fence at the moment it
//          observes the miss, then re-checks for EXACT equality after
//          the unlocked backing-store read returns; if the fence moved
//          at all, something in this shard changed while the read was
//          in flight, and the read result is discarded rather than
//          cached — this is what stops a read from resurrecting data
//          that a concurrent Invalidate() just durably removed (an
//          absent -> present -> absent "ABA" cycle that a simple
//          presence check alone cannot detect).
//       2) Reserved sentinel version 0 for read-fills: even when the
//          fence is unchanged (proving the key is still cache-absent),
//          a real Put() for the SAME key may already have been assigned
//          a version number (via journalMutex_ + globalVersionCounter_)
//          without yet having reached its own shard-commit step — i.e.
//          it is real, already-durable, but "in flight" with respect to
//          the in-memory shard. If Get()'s read-fill minted a FRESH,
//          numerically higher version for itself (the naive fix), that
//          in-flight write would later arrive, compare its own (lower,
//          but chronologically earlier-assigned) version against the
//          fill's, and be wrongly discarded by the existing
//          "version <= existing -> skip" guard — silently losing a real
//          write. Using the reserved value 0 (never assigned to any
//          real write; AppendJournalRecordWithNewVersion always returns
//          >= 1) avoids this entirely: 0 can never equal-or-exceed a
//          real write's version, so any real write — whether it arrives
//          before or after a read-fill — always wins the comparison and
//          correctly overwrites a stale fill, with no ordering
//          assumptions required. Fresh inserts (isUpdate == false) are
//          not gated by the version comparison at all, so stamping a
//          read-fill's OWN insert with 0 is always safe at the moment it
//          happens; the reserved value only matters for what happens
//          afterward, when a real write for the same key is applied on
//          top of it.
//     Invalidate()/ForceInvalidate() got the equivalent fix: they now
//     mint their own fresh version (like Put) and only remove an entry
//     whose version is strictly OLDER than that mint, rather than
//     unconditionally erasing "whatever key currently maps to" — see
//     InvalidateImpl.
#include "QuantumCache/CoreEngine/ICacheEngine.h"
#include "QuantumCache/CoreEngine/JournalRecordCodec.h"
#include "QuantumCache/Storage/IBackingStore.h"
#include "QuantumCache/PowerResilience/IWriteAheadJournal.h"
#include "QuantumCache/Logging/ILogger.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace QuantumCache::CoreEngine {
namespace {

using Common::Error;
using Common::ErrorCode;
using Common::Result;

// Fixed per-entry bookkeeping overhead added on top of key+value byte
// counts when accounting memory usage against CacheEngineOptions'
// capacity budget. This is a deliberate, documented approximation (real
// allocator/map/list overhead varies) rather than an attempt at
// byte-exact RSS accounting, which is out of scope for Stage 2.
constexpr std::size_t kEntryOverheadBytes = 64;

// Same sanity bound used by JournalRecordCodec, applied here to values
// read back from the backing store — this is the "validates the result"
// step of the read path: a defensive check independent of whatever
// integrity checking the backing store itself does internally.
constexpr std::size_t kMaxReasonableValueBytes = 64ull * 1024 * 1024;

enum class EngineLifecycle : int {
    NotReady = 0,
    Ready = 1,
    Stopping = 2,
    Stopped = 3,
    // AUDITED BUG (fixed): previously there was no distinct terminal
    // state for "ReplayFromJournal() failed". A failed replay left
    // lifecycle_ sitting at NotReady, and MarkRecoveryComplete() only
    // checked `current != NotReady` to reject calls made "too late" —
    // which meant a caller that (incorrectly, but not impossibly, e.g.
    // due to a bug in its own error handling) called
    // MarkRecoveryComplete() after a FAILED ReplayFromJournal() would
    // succeed in transitioning the engine to Ready, silently exposing
    // whatever partially-recovered/inconsistent in-memory state existed
    // at the point replay aborted. RecoveryFailed is a permanent,
    // latching terminal state: once set, MarkRecoveryComplete() always
    // refuses, and CheckReadyForRead/CheckReadyForWrite always refuse,
    // for the remaining lifetime of this engine instance. This is
    // defense-in-depth: RecoveryManager (see RecoveryManager.cpp) is
    // already expected to never call MarkRecoveryComplete() after a
    // failed replay, but the cache engine must not rely solely on its
    // caller behaving correctly for a safety-critical invariant.
    RecoveryFailed = 4,
};

struct Entry {
    std::string key;
    std::vector<std::uint8_t> value;
    std::uint64_t version{0};
    EntryDirtyState dirtyState{EntryDirtyState::Clean};

    [[nodiscard]] std::size_t SizeBytes() const noexcept {
        return key.size() + value.size() + kEntryOverheadBytes;
    }
};

using LruList = std::list<Entry>;

struct Shard {
    mutable std::mutex mutex;
    LruList lru; // front = most recently used, back = least recently used
    std::unordered_map<std::string, LruList::iterator> index;
    std::uint64_t currentBytes{0};
    std::uint64_t dirtyBytes{0};

    // Incremented under `mutex`, exactly once, every time a real
    // write/removal actually commits a value-changing mutation to this
    // shard's live state — i.e. every non-discarded InsertOrUpdateLocked
    // call and every InvalidateImpl erase. NEVER incremented by eviction
    // (a capacity side-effect of some OTHER key's insert, which already
    // bumps the fence itself) and NEVER incremented by a read-fill.
    // Get()'s cache-miss path uses this as a lightweight, O(1),
    // bounded-memory "did anything in this shard change while I was
    // doing unlocked backing-store I/O" fence — see the "Read-fill
    // linearization" file-header comment and Get()'s implementation for
    // the full correctness argument (this is the fix for the audited
    // read-miss/write race).
    std::uint64_t mutationFence{0};
    std::size_t dirtyCount{0};
};

class CacheEngine final : public ICacheEngine {
public:
    CacheEngine(CacheEngineOptions options,
                std::shared_ptr<Storage::IBackingStore> backingStore,
                std::shared_ptr<PowerResilience::IWriteAheadJournal> journal,
                std::shared_ptr<Logging::ILogger> logger)
        : options_(options),
          backingStore_(std::move(backingStore)),
          journal_(std::move(journal)),
          logger_(std::move(logger)),
          shards_(std::max<std::uint32_t>(1, options.shardCount)) {
        perShardCapacityBytes_ = options_.capacityBytes / shards_.size();
        perShardMaxEntries_ = std::max<std::uint64_t>(1, options_.maxEntryCount / shards_.size());
    }

    // Stops and joins the periodic background-flush thread, if one was
    // started. Safe to call multiple times (idempotent) and safe to call
    // even if the thread was never started. This is invoked from both
    // Shutdown() (the normal, documented path) and the destructor (a
    // safety net so a CacheEngine that is destroyed WITHOUT an explicit
    // Shutdown() call — e.g. a test fixture, or a bug in caller code —
    // never leaves a background thread running against a
    // partially-destroyed object, which would be undefined behavior, not
    // merely a resource leak).
    ~CacheEngine() override {
        StopBackgroundFlushThread();
    }

    // -----------------------------------------------------------------
    // Recovery-time methods.
    // -----------------------------------------------------------------

    Result<void> ReplayFromJournal() override {
        if (lifecycle_.load() != EngineLifecycle::NotReady) {
            return Result<void>::Failure(Error{
                ErrorCode::InvalidArgument,
                "ReplayFromJournal() may only be called before MarkRecoveryComplete()", 0});
        }

        // Idempotent: rebuild reconstruction state from scratch every
        // call rather than accumulating across calls.
        struct PendingEntry {
            std::vector<std::uint8_t> value;
            std::uint64_t version{0};
            EntryDirtyState state{EntryDirtyState::Dirty};
        };
        std::unordered_map<std::string, PendingEntry> pending;

        auto replayResult = journal_->Replay(
            [&](const PowerResilience::JournalRecord& raw) -> Result<PowerResilience::ReplayAction> {
                auto decoded = JournalRecordCodec::Decode(raw.payload);
                if (!decoded) {
                    // A structurally-valid journal frame (already CRC
                    // checked by IWriteAheadJournal) that fails to decode
                    // as a cache record is a real corruption of a
                    // different kind than a torn write, and must not be
                    // silently ignored.
                    return Result<PowerResilience::ReplayAction>::Failure(decoded.Err());
                }

                const CacheJournalRecord& record = decoded.Value();
                switch (record.type) {
                    case CacheRecordType::Upsert:
                        pending[record.key] = PendingEntry{
                            record.value, record.entryVersion, EntryDirtyState::Dirty};
                        break;

                    case CacheRecordType::FlushIntent: {
                        auto it = pending.find(record.key);
                        if (it != pending.end() && it->second.version == record.entryVersion) {
                            it->second.state = EntryDirtyState::FlushInProgress;
                        }
                        break;
                    }

                    case CacheRecordType::FlushComplete: {
                        auto it = pending.find(record.key);
                        if (it != pending.end() && it->second.version == record.entryVersion) {
                            // Fully durable in the backing store now; no
                            // need to carry it as dirty cache state.
                            pending.erase(it);
                        }
                        break;
                    }

                    case CacheRecordType::Invalidate: {
                        pending.erase(record.key);
                        // Idempotent regardless of whether the crash
                        // happened before or after the original
                        // backing-store removal completed — but the
                        // RESULT of this call must still be checked.
                        // AUDITED BUG (fixed): this used to be
                        // `(void)backingStore_->Remove(...)`, silently
                        // discarding [[nodiscard]] failures. If the
                        // backing store cannot durably confirm a removal
                        // during replay (e.g. disk I/O error, out of
                        // space, media failure), the cache MUST NOT
                        // silently proceed as though recovery succeeded
                        // — that could leave the backing store holding a
                        // value that the cache (and the rest of the
                        // system) believes was removed, a real
                        // correctness violation, not merely a
                        // performance concern. Surface this as a hard
                        // replay failure so InitializeAndRecover()
                        // reports RecoveryFailed instead of
                        // RecoveryComplete.
                        auto removeResult = backingStore_->Remove(record.key);
                        if (!removeResult) {
                            return Result<PowerResilience::ReplayAction>::Failure(
                                Error{ErrorCode::RecoveryFailed,
                                      "journal replay: backing store Remove() failed for key '" +
                                          record.key + "' during Invalidate replay: " +
                                          removeResult.Err().message,
                                      removeResult.Err().platformErrorValue});
                        }
                        break;
                    }
                }

                return Result<PowerResilience::ReplayAction>::Success(
                    PowerResilience::ReplayAction::Continue);
            });

        if (!replayResult) {
            // Latch the permanent failure state (see EngineLifecycle's
            // RecoveryFailed comment) so no subsequent
            // MarkRecoveryComplete() call — however it gets invoked —
            // can ever transition this engine instance to Ready.
            lifecycle_.store(EngineLifecycle::RecoveryFailed);
            return replayResult;
        }

        // Anything still pending after processing every valid record was
        // durably journaled (an Upsert exists) but never confirmed
        // flushed (no matching FlushComplete). Re-insert it into the live
        // cache as Dirty — regardless of whether it was mid-flush
        // (FlushInProgress) at crash time, since a FlushIntent alone does
        // not prove the backing-store write landed. This is the concrete
        // mechanism that makes a write survive "process crash / forced
        // termination / interrupted write": the value itself came from
        // the Upsert record, not from any assumption about how far the
        // flush got.
        std::uint64_t highestVersionSeen = 0;
        for (auto& [key, pendingEntry] : pending) {
            Shard& shard = ShardFor(key);
            std::lock_guard<std::mutex> lock(shard.mutex);
            InsertOrUpdateLocked(shard, key, pendingEntry.value, pendingEntry.version,
                                  EntryDirtyState::Dirty, /*enforceCapacity=*/false);
            highestVersionSeen = std::max(highestVersionSeen, pendingEntry.version);
        }

        globalVersionCounter_.store(std::max(globalVersionCounter_.load(), highestVersionSeen));

        if (logger_ && !pending.empty()) {
            logger_->Log(Logging::LogLevel::Warning, "CoreEngine",
                         "Recovered " + std::to_string(pending.size()) +
                             " dirty entr" + (pending.size() == 1 ? std::string("y") : std::string("ies")) +
                             " from write-ahead journal after unclean shutdown.");
        }

        return Result<void>::Success();
    }

    Result<void> MarkRecoveryComplete() override {
        auto current = lifecycle_.load();
        if (current == EngineLifecycle::Ready) {
            return Result<void>::Success(); // idempotent no-op
        }
        // AUDITED BUG (fixed): see EngineLifecycle::RecoveryFailed's
        // comment. This must be checked as its own explicit case (never
        // permitted to fall through to "success"), independent of
        // whatever the caller's own control flow does after a failed
        // ReplayFromJournal() call.
        if (current == EngineLifecycle::RecoveryFailed) {
            return Result<void>::Failure(Error{
                ErrorCode::RecoveryFailed,
                "cannot mark recovery complete: a prior ReplayFromJournal() call failed and "
                "this engine instance is permanently unusable for its remaining lifetime", 0});
        }
        if (current != EngineLifecycle::NotReady) {
            return Result<void>::Failure(Error{
                ErrorCode::InvalidArgument,
                "cannot mark recovery complete after the engine has begun shutting down", 0});
        }
        lifecycle_.store(EngineLifecycle::Ready);
        if (logger_) {
            logger_->Log(Logging::LogLevel::Info, "CoreEngine",
                         "Cache engine is now READY (recovery complete).");
        }

        // Only started here, i.e. only after recovery has genuinely
        // completed — never before, so the background flusher can never
        // observe or act on not-yet-replayed state.
        StartBackgroundFlushThreadIfConfigured();

        return Result<void>::Success();
    }

    // -----------------------------------------------------------------
    // Read path.
    // -----------------------------------------------------------------

    // ---------------------------------------------------------------
    // Read-miss/concurrent-write race fix.
    //
    // AUDITED BUG (now fixed): the original implementation minted a
    // version for a cache-miss read-fill via
    // `globalVersionCounter_.fetch_add(1, ...) + 1`, taken AFTER the
    // unlocked backing-store read returned. That number has no causal
    // relationship to a concurrent Put() for the same key — it only
    // reflects which thread happened to reach that line of code later in
    // wall-clock time. Concretely:
    //   T1: Get(k) misses, releases shard lock, calls backingStore_->Get(k)
    //       (still returns the OLD value; this read is now "in flight").
    //   T2: Put(k, new) runs to completion: mints version N via
    //       journalMutex_, durably journals it, and commits a Dirty
    //       entry with version N into the shard.
    //   T1: backingStore_->Get(k) returns the OLD value. T1 then minted
    //       ITS OWN version via the raw counter — which, having been
    //       incremented after T2's, could easily be N+1, N+5, etc. — and
    //       called InsertOrUpdateLocked(..., version=N+1, Clean, ...).
    //       InsertOrUpdateLocked's guard is `if (version <= existing)
    //       return;`, so N+1 > N passes the guard, and the STALE old
    //       value silently overwrites the newer Dirty entry, clearing
    //       its dirty flag in the process. The write itself is NOT lost
    //       (it is durably in the journal and would be recovered on
    //       restart), but the LIVE cache now serves stale data and has
    //       forgotten the entry needs to be flushed — a real correctness
    //       violation of "the stale read result must never overwrite or
    //       be returned instead of a newer dirty cache entry."
    //
    // THE FIX has two parts, both required (see the file-header
    // "Read-fill linearization" comment for why one alone is not
    // sufficient):
    //   1) Shard::mutationFence — a plain counter bumped under shard.mutex
    //      by every REAL commit (insert/update/removal), snapshotted by
    //      Get() before the unlocked read and re-checked for EXACT
    //      equality afterward. If it moved at all, some real mutation
    //      happened for (at least) some key in this shard while the read
    //      was in flight, and the read is conservatively treated as
    //      possibly-stale and discarded — never cached, though still
    //      returned to the caller as a correct point-in-time answer (see
    //      below). This specifically stops the stale-overwrite scenario
    //      above, AND additionally stops the more dangerous "resurrect a
    //      just-invalidated key" scenario (Invalidate() removes an
    //      entry entirely, so a plain "does an Entry still exist"
    //      re-check can't detect an absent -> present -> absent cycle;
    //      the fence can).
    //   2) The reserved sentinel version 0 for whatever a read-fill DOES
    //      still insert. AppendJournalRecordWithNewVersion always
    //      returns versions >= 1, so 0 can never equal-or-exceed a real
    //      write's version — meaning ANY real Put()/Invalidate() for the
    //      same key, whether it commits before or after this read-fill,
    //      is guaranteed to correctly supersede it. This removes any
    //      remaining dependence on wall-clock ordering between the
    //      read's own fill and a real write for the same key that
    //      commits later (the fence in part 1 only protects against
    //      writes that already committed among the ones this read
    //      raced against by the time it re-checks).
    Result<std::vector<std::uint8_t>> Get(const std::string& key) override {
        auto readyCheck = CheckReadyForRead();
        if (!readyCheck) return Result<std::vector<std::uint8_t>>::Failure(readyCheck.Err());
        if (!options_.enabled) {
            return Result<std::vector<std::uint8_t>>::Failure(
                Error{ErrorCode::CacheDisabled, "cache is disabled by configuration", 0});
        }

        Shard& shard = ShardFor(key);
        std::uint64_t fenceAtMiss = 0;
        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            auto it = shard.index.find(key);
            if (it != shard.index.end()) {
                // Cache hit: move to front (most recently used) and
                // return a copy of the cached bytes.
                shard.lru.splice(shard.lru.begin(), shard.lru, it->second);
                hitCount_.fetch_add(1, std::memory_order_relaxed);
                return Result<std::vector<std::uint8_t>>::Success(it->second->value);
            }
            // Snapshot the fence WHILE still holding the lock that
            // established the miss, so there is no gap between
            // "observed absent" and "started watching for changes."
            fenceAtMiss = shard.mutationFence;
        }

        // Cache miss: fall through to the backing store, OUTSIDE the
        // shard lock (this is real disk I/O and must never block other
        // keys in the same shard).
        missCount_.fetch_add(1, std::memory_order_relaxed);

        auto backingResult = backingStore_->Get(key);
        if (!backingResult) {
            // NotFound is a legitimate outcome (key genuinely does not
            // exist anywhere); any other error is a real backing-store
            // problem and must be surfaced, not swallowed.
            return Result<std::vector<std::uint8_t>>::Failure(backingResult.Err());
        }

        // Validate the result before trusting/caching it.
        if (backingResult.Value().size() > kMaxReasonableValueBytes) {
            return Result<std::vector<std::uint8_t>>::Failure(Error{
                ErrorCode::CorruptData,
                "backing store returned an implausibly large value; refusing to cache or trust it", 0});
        }

        // Commit phase. Two distinct correctness requirements are
        // enforced here together:
        //   (a) never OVERWRITE a newer cache entry with this stale
        //       backing-store snapshot;
        //   (b) never RETURN this stale snapshot to the caller if a
        //       newer entry for the same key now exists in the cache —
        //       the caller must see the newer value instead.
        // Both are handled by re-checking the shard under lock: if the
        // fence hasn't moved, nothing raced us and it is safe to both
        // insert AND return the backing-store value we read. If the
        // fence DID move, some real mutation happened for (at least)
        // some key in this shard while our read was in flight; we then
        // look the key up again — if a live entry exists now, that is
        // necessarily at least as new as anything we could have raced
        // (see reserved-sentinel-version rationale above), so we return
        // ITS value instead of our stale one, satisfying (b) without
        // touching/overwriting it, satisfying (a).
        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            if (shard.mutationFence == fenceAtMiss) {
                // Nothing changed since the miss was observed: safe to
                // insert AND to return this read's value. Reserved
                // sentinel version (0): see part 2 of the fix rationale
                // above. isUpdate is necessarily false here (nothing
                // changed since we observed the key absent), so this is
                // always a safe fresh insert; the sentinel value only
                // matters for how a LATER real write for this same key
                // is correctly able to supersede it.
                InsertOrUpdateLocked(shard, key, backingResult.Value(), /*version=*/0,
                                      EntryDirtyState::Clean, /*enforceCapacity=*/true);
                return Result<std::vector<std::uint8_t>>::Success(backingResult.Value());
            }

            // The shard changed while this read was in flight (a
            // concurrent write, invalidate, or another thread's own
            // read-fill of a DIFFERENT key in the same shard all bump
            // the fence). Do NOT insert our stale snapshot. Check
            // whether a live entry exists now for THIS key — if so, it
            // is guaranteed newer than what we could have raced (every
            // real write's version is >= 1, strictly greater than our
            // never-used sentinel 0; a fresher read-fill from another
            // thread would itself have gone through this same
            // fence-checked path), so return that instead of our stale
            // read, satisfying the "never returned instead of a newer
            // entry" requirement.
            auto it = shard.index.find(key);
            if (it != shard.index.end()) {
                shard.lru.splice(shard.lru.begin(), shard.lru, it->second);
                hitCount_.fetch_add(1, std::memory_order_relaxed);
                return Result<std::vector<std::uint8_t>>::Success(it->second->value);
            }
        }

        // No live entry exists for this key even after the re-check
        // (e.g. the fence moved because of a concurrent Invalidate() of
        // THIS key, or activity on an unrelated key in the same shard,
        // and nothing has replaced it since). Our stale backing-store
        // read is not cached, but is still the best available answer to
        // return to the caller — it is a genuine value that was
        // authoritative at some real point in time, simply not cached.
        return Result<std::vector<std::uint8_t>>::Success(backingResult.Value());
    }

    bool Contains(const std::string& key) override {
        // AUDITED BUG (fixed): this method used to have NO recovery-state
        // gate at all — it happily reported cache/backing-store presence
        // even before ReplayFromJournal()/MarkRecoveryComplete() had run,
        // and even after Shutdown() completed. That is a real violation
        // of the documented, load-bearing contract ("no data-plane method
        // may expose normal cache state before recovery is complete").
        // Contains() returns a plain bool (not a Result<bool>) by
        // interface design, so the fail-safe answer when not ready is
        // simply `false` — never claim a key is present when the
        // recovery-gated state backing that answer cannot yet (or can no
        // longer) be trusted.
        if (!CheckReadyForRead()) {
            return false;
        }
        Shard& shard = ShardFor(key);
        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            if (shard.index.count(key) > 0) return true;
        }
        return backingStore_->Contains(key);
    }

    Result<CacheEntryInfo> GetEntryInfo(const std::string& key) const override {
        // AUDITED BUG (fixed): like Contains() above, this method used
        // to have NO recovery-state gate — a caller could inspect
        // in-memory entry metadata (version, size, dirty state) before
        // ReplayFromJournal() had finished reconstructing it, or after
        // Shutdown() had already torn it down. Enforce the same
        // recovery-lifecycle contract every other data-plane method
        // enforces.
        auto readyCheck = CheckReadyForRead();
        if (!readyCheck) return Result<CacheEntryInfo>::Failure(readyCheck.Err());

        const Shard& shard = ShardFor(key);
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto it = shard.index.find(key);
        if (it == shard.index.end()) {
            return Result<CacheEntryInfo>::Failure(
                Error{ErrorCode::NotFound, "key not present in cache (may still be in backing store)", 0});
        }
        CacheEntryInfo info;
        info.key = key;
        info.version = it->second->version;
        info.sizeBytes = it->second->value.size();
        info.dirtyState = it->second->dirtyState;
        return Result<CacheEntryInfo>::Success(info);
    }

    // -----------------------------------------------------------------
    // Write path.
    // -----------------------------------------------------------------

    Result<void> Put(const std::string& key, const std::vector<std::uint8_t>& value) override {
        auto readyCheck = CheckReadyForWrite();
        if (!readyCheck) return readyCheck;
        if (!options_.enabled) {
            return Result<void>::Failure(
                Error{ErrorCode::CacheDisabled, "cache is disabled by configuration", 0});
        }

        // AUDITED BUG (fixed): admission MUST happen here, immediately
        // after the Ready check and before any journal/shard mutation —
        // see TryAdmitWrite()'s comment for the exact race this closes
        // against a concurrent Shutdown(). If shutdown began in the
        // (tiny) window between this Put() call's own CheckReadyForWrite()
        // above and this admission attempt, TryAdmitWrite() itself
        // re-checks lifecycle_ under the same mutex Shutdown() uses to
        // flip it, so this fails closed rather than racing.
        if (!TryAdmitWrite()) {
            return Result<void>::Failure(Error{
                ErrorCode::ServiceStopping,
                "cache engine is shutting down and no longer accepts new writes", 0});
        }
        WriteAdmissionGuard admissionGuard(*this);

        CacheJournalRecord record;
        record.type = CacheRecordType::Upsert;
        record.key = key;
        record.value = value;

        std::uint64_t version = 0;
        auto appendResult = AppendJournalRecordWithNewVersion(record, version);
        if (!appendResult) {
            // The write was NOT durably journaled. The cache must not be
            // mutated, and the caller must not be told this succeeded.
            return appendResult;
        }

        Shard& shard = ShardFor(key);
        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            InsertOrUpdateLocked(shard, key, value, version, EntryDirtyState::Dirty,
                                  /*enforceCapacity=*/true);
        }

        RelieveCapacityPressureIfNeeded(shard);

        if (options_.writePolicy == WritePolicyKind::WriteThrough) {
            // Confirm the write reaches the backing store before Put()
            // itself returns success, per the WriteThrough contract. A
            // failure here is reported to the caller, but the data is
            // NOT lost — it remains durably Dirty in the journal/cache
            // and will be retried by a later Flush()/FlushAll()/recovery.
            auto flushResult = FlushKey(key, version);
            if (!flushResult) {
                return flushResult;
            }
        }

        return Result<void>::Success();
    }

    Result<void> Invalidate(const std::string& key) override {
        return InvalidateImpl(key, /*force=*/false);
    }

    Result<void> ForceInvalidate(const std::string& key) override {
        return InvalidateImpl(key, /*force=*/true);
    }

    // -----------------------------------------------------------------
    // Deferred-write / flush path.
    // -----------------------------------------------------------------

    Result<void> Flush(const std::string& key) override {
        auto readyCheck = CheckReadyForRead(); // flushing is safe during Stopping (drain), not before recovery
        if (!readyCheck) return Result<void>::Failure(readyCheck.Err());
        return FlushKey(key, /*expectedVersion=*/0, /*checkVersion=*/false);
    }

    Result<void> FlushAll() override {
        auto readyCheck = CheckReadyForRead();
        if (!readyCheck) return Result<void>::Failure(readyCheck.Err());

        Error lastError;
        bool anyFailed = false;

        for (auto& shard : shards_) {
            std::vector<std::string> dirtyKeys;
            {
                std::lock_guard<std::mutex> lock(shard.mutex);
                dirtyKeys.reserve(shard.dirtyCount);
                for (auto& entry : shard.lru) {
                    if (entry.dirtyState == EntryDirtyState::Dirty) {
                        dirtyKeys.push_back(entry.key);
                    }
                }
            }

            for (auto& key : dirtyKeys) {
                auto result = FlushKey(key, 0, /*checkVersion=*/false);
                if (!result) {
                    anyFailed = true;
                    lastError = result.Err();
                    if (logger_) {
                        logger_->Log(Logging::LogLevel::Error, "CoreEngine",
                                     "FlushAll: failed to flush key '" + key + "': " + result.Err().message);
                    }
                }
            }
        }

        if (anyFailed) {
            return Result<void>::Failure(lastError);
        }

        // Priority 2.7 (journal growth/compaction): opportunistically
        // attempt to reclaim journal space now that every dirty entry
        // this FlushAll() found has been successfully flushed. This is
        // the ONLY place besides Shutdown() the journal is ever
        // truncated — see TryCompactJournalIfFullyClean()'s full
        // rationale. Deliberately unconditional (not gated behind any
        // "only if it's been a while" heuristic) because the check
        // itself is cheap (a handful of already-necessary shard-lock
        // acquisitions) and TryCompactJournalIfFullyClean() itself is a
        // correct no-op whenever anything is still dirty.
        TryCompactJournalIfFullyClean();
        return Result<void>::Success();
    }

    // -----------------------------------------------------------------
    // Observability.
    // -----------------------------------------------------------------

    CacheStatistics GetStatistics() const noexcept override {
        CacheStatistics stats;
        stats.hitCount = hitCount_.load(std::memory_order_relaxed);
        stats.missCount = missCount_.load(std::memory_order_relaxed);
        stats.insertCount = insertCount_.load(std::memory_order_relaxed);
        stats.updateCount = updateCount_.load(std::memory_order_relaxed);
        stats.invalidationCount = invalidationCount_.load(std::memory_order_relaxed);
        stats.evictionCount = evictionCount_.load(std::memory_order_relaxed);
        stats.flushSuccessCount = flushSuccessCount_.load(std::memory_order_relaxed);
        stats.flushFailureCount = flushFailureCount_.load(std::memory_order_relaxed);

        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            stats.currentEntryCount += shard.index.size();
            stats.currentMemoryBytes += shard.currentBytes;
            stats.dirtyEntryCount += shard.dirtyCount;
            stats.dirtyBytes += shard.dirtyBytes;
        }
        return stats;
    }

    // -----------------------------------------------------------------
    // Shutdown.
    // -----------------------------------------------------------------

    Result<void> Shutdown() override {
        EngineLifecycle expected = EngineLifecycle::Ready;
        if (!lifecycle_.compare_exchange_strong(expected, EngineLifecycle::Stopping)) {
            if (expected == EngineLifecycle::Stopped || expected == EngineLifecycle::Stopping) {
                return Result<void>::Success(); // already shutting down / shut down: idempotent
            }
            return Result<void>::Failure(Error{
                ErrorCode::InvalidArgument, "cannot shut down an engine that was never marked ready", 0});
        }

        if (logger_) {
            logger_->Log(Logging::LogLevel::Info, "CoreEngine",
                         "Cache engine shutting down: no longer accepting new writes; flushing dirty entries.");
        }

        // AUDITED BUG (fixed): must wait for every write that was
        // ALREADY ADMITTED (via TryAdmitWrite()) before the
        // compare_exchange_strong above to fully finish committing to
        // shard state, before doing anything below that inspects shard
        // dirty counts or truncates the journal. Safe to call now
        // (rather than deadlocking): lifecycle_ is already off Ready, so
        // TryAdmitWrite() cannot admit any NEW writer from this point
        // on — inFlightWriteCount_ can therefore only decrease, and this
        // wait is bounded by however long the (journal-append-already-
        // done, shard-lock-only) remainder of an admitted Put()/
        // InvalidateImpl() call takes, never by new work arriving.
        // Without this, FlushAll() below could run BEFORE an admitted
        // writer's value is even visible in any shard's LRU (so
        // FlushAll() cannot flush it), and the later dirtyCount==0 check
        // could then incorrectly authorize journal_->Truncate(),
        // destroying that writer's already-durably-appended record out
        // from under it — silently losing a write the caller was told
        // succeeded. See CacheEngineShutdownRaceTests for the regression
        // coverage of this exact interleaving.
        DrainInFlightWrites();

        // Stop the periodic background flusher BEFORE the final,
        // authoritative FlushAll() below, so there is no window where the
        // background thread's own FlushAll() runs concurrently with (and
        // duplicates work already being done by) the shutdown flush.
        // FlushAll() itself is safe to call concurrently (see
        // ConcurrentFlushAllCalls_DoNotCorruptStatisticsOrData), so this
        // ordering is a scheduling/efficiency choice, not a correctness
        // requirement — but it keeps shutdown logs from being confusing.
        StopBackgroundFlushThread();

        auto flushResult = FlushAll();
        if (!flushResult && logger_) {
            logger_->Log(Logging::LogLevel::Warning, "CoreEngine",
                         "Shutdown: FlushAll did not fully succeed (" + flushResult.Err().message +
                             "). Unflushed entries remain safely durable in the write-ahead journal "
                             "and will be recovered on next startup.");
        }

        lifecycle_.store(EngineLifecycle::Stopped);

        // Only safe to truncate the journal if EVERY entry is confirmed
        // Clean — truncating while any dirty data remains would destroy
        // the only durable record of that data. This check is the load-
        // bearing safety invariant for this method; see
        // CacheEngineShutdownTests for the regression test.
        std::uint64_t remainingDirty = 0;
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            remainingDirty += shard.dirtyCount;
        }

        if (remainingDirty == 0) {
            std::lock_guard<std::mutex> journalLock(journalMutex_);
            auto truncateResult = journal_->Truncate();
            if (!truncateResult && logger_) {
                logger_->Log(Logging::LogLevel::Warning, "CoreEngine",
                             "Shutdown: journal truncation failed (" + truncateResult.Err().message +
                                 "); journal will simply be replayed (as a no-op) next startup.");
            }
        } else if (logger_) {
            logger_->Log(Logging::LogLevel::Warning, "CoreEngine",
                         std::to_string(remainingDirty) +
                             " entries remain dirty at shutdown; journal NOT truncated so they "
                             "remain recoverable.");
        }

        return Result<void>::Success();
    }

private:
    // -------------------- internal helpers --------------------

    [[nodiscard]] Shard& ShardFor(const std::string& key) noexcept {
        return shards_[std::hash<std::string>{}(key) % shards_.size()];
    }
    [[nodiscard]] const Shard& ShardFor(const std::string& key) const noexcept {
        return shards_[std::hash<std::string>{}(key) % shards_.size()];
    }

    [[nodiscard]] Result<void> CheckReadyForRead() const {
        auto state = lifecycle_.load();
        if (state == EngineLifecycle::NotReady) {
            return Result<void>::Failure(Error{
                ErrorCode::RecoveryNotComplete,
                "cache engine is not ready: recovery has not completed", 0});
        }
        // AUDITED BUG (fixed): RecoveryFailed used to not exist as a
        // distinct state, so this check (which only tested for
        // NotReady/Stopped) would fall through to Success() for a
        // failed-replay engine — meaning Get()/Contains()/etc. could
        // read from a permanently-unusable engine instance as though it
        // were healthy. RecoveryFailed must always refuse reads.
        if (state == EngineLifecycle::RecoveryFailed) {
            return Result<void>::Failure(Error{
                ErrorCode::RecoveryFailed,
                "cache engine recovery failed; this instance is permanently unusable", 0});
        }
        if (state == EngineLifecycle::Stopped) {
            return Result<void>::Failure(Error{
                ErrorCode::ServiceStopping, "cache engine has finished shutting down", 0});
        }
        return Result<void>::Success();
    }

    [[nodiscard]] Result<void> CheckReadyForWrite() const {
        auto state = lifecycle_.load();
        if (state == EngineLifecycle::NotReady) {
            return Result<void>::Failure(Error{
                ErrorCode::RecoveryNotComplete,
                "cache engine is not ready: recovery has not completed", 0});
        }
        // AUDITED BUG (fixed): see CheckReadyForRead's comment — report
        // the specific RecoveryFailed reason rather than the generic
        // "shutting down" (ServiceStopping) message a RecoveryFailed
        // engine used to fall through to.
        if (state == EngineLifecycle::RecoveryFailed) {
            return Result<void>::Failure(Error{
                ErrorCode::RecoveryFailed,
                "cache engine recovery failed; this instance is permanently unusable", 0});
        }
        if (state != EngineLifecycle::Ready) {
            return Result<void>::Failure(Error{
                ErrorCode::ServiceStopping,
                "cache engine is shutting down and no longer accepts new writes", 0});
        }
        return Result<void>::Success();
    }

    // Increments the global version counter and appends `record` (with
    // that version stamped in) to the journal, all while holding
    // journalMutex_, so version order is guaranteed to match journal
    // append order across every thread. Returns the assigned version via
    // `outVersion` on success.
    [[nodiscard]] Result<void> AppendJournalRecordWithNewVersion(
        CacheJournalRecord record, std::uint64_t& outVersion) {
        std::lock_guard<std::mutex> lock(journalMutex_);
        outVersion = globalVersionCounter_.fetch_add(1, std::memory_order_relaxed) + 1;
        record.entryVersion = outVersion;
        auto encoded = JournalRecordCodec::Encode(record);
        auto appendResult = journal_->Append(encoded);
        if (!appendResult) {
            return Result<void>::Failure(appendResult.Err());
        }
        return Result<void>::Success();
    }

    [[nodiscard]] Result<void> AppendJournalRecord(const CacheJournalRecord& record) {
        std::lock_guard<std::mutex> lock(journalMutex_);
        auto encoded = JournalRecordCodec::Encode(record);
        auto appendResult = journal_->Append(encoded);
        if (!appendResult) {
            return Result<void>::Failure(appendResult.Err());
        }
        return Result<void>::Success();
    }

    // Applies an insert/update to `shard` (already locked by caller),
    // enforcing capacity afterward if requested. Used by the write path
    // (Dirty), the read-miss path (Clean), and journal replay (Dirty,
    // capacity NOT enforced — see ReplayFromJournal).
    void InsertOrUpdateLocked(Shard& shard, const std::string& key,
                               const std::vector<std::uint8_t>& value, std::uint64_t version,
                               EntryDirtyState state, bool enforceCapacity) {
        auto it = shard.index.find(key);
        bool isUpdate = it != shard.index.end();

        if (isUpdate) {
            // Optimistic-versioning guard: never let an older write
            // clobber a newer one that already landed (see file header
            // comment on version assignment). Note version 0 is the
            // reserved read-fill sentinel (see Get()'s fix rationale) —
            // it can only ever equal 0 here if `it->second->version` is
            // also 0 (an existing read-fill being replaced by a NEWER
            // read-fill snapshot of the same still-uncommitted-elsewhere
            // key), which is fine to allow through as a same-priority
            // refresh; any REAL write's version (>= 1) always compares
            // strictly greater than a resident 0 and correctly wins.
            if (version <= it->second->version && !(version == 0 && it->second->version == 0)) {
                return;
            }
            shard.currentBytes -= it->second->SizeBytes();
            if (it->second->dirtyState != EntryDirtyState::Clean) {
                shard.dirtyBytes -= it->second->SizeBytes();
                --shard.dirtyCount;
            }
            shard.lru.erase(it->second);
            shard.index.erase(it);
        }

        Entry entry;
        entry.key = key;
        entry.value = value;
        entry.version = version;
        entry.dirtyState = state;
        std::size_t sizeBytes = entry.SizeBytes();

        shard.lru.push_front(std::move(entry));
        shard.index[key] = shard.lru.begin();
        shard.currentBytes += sizeBytes;
        if (state != EntryDirtyState::Clean) {
            shard.dirtyBytes += sizeBytes;
            ++shard.dirtyCount;
        }

        if (isUpdate) {
            updateCount_.fetch_add(1, std::memory_order_relaxed);
        } else {
            insertCount_.fetch_add(1, std::memory_order_relaxed);
        }

        // A real, committed mutation of this shard's live state just
        // happened — advance the fence Get()'s read-fill path uses to
        // detect "something changed while my backing-store read was in
        // flight." Must be bumped for every commit reached this point
        // (both fresh inserts and version-superseding updates), and must
        // NOT be bumped by the early "stale/no-op" return above.
        ++shard.mutationFence;

        if (enforceCapacity) {
            EvictCleanEntriesLocked(shard);
        }
    }

    // LRU eviction over CLEAN entries only — dirty/flush-in-progress
    // entries are never evicted (that would silently destroy the only
    // in-memory copy of data that has not yet reached the backing store;
    // the journal still has it, but evicting would defeat the point of
    // the cache holding it). Runs entirely in-memory: no I/O, so it is
    // safe to call while holding the shard lock.
    void EvictCleanEntriesLocked(Shard& shard) {
        while ((shard.currentBytes > perShardCapacityBytes_ ||
                shard.index.size() > perShardMaxEntries_) &&
               shard.index.size() > shard.dirtyCount) {
            // Find the least-recently-used Clean entry, scanning from the
            // back. Documented O(n) worst case in a shard is an accepted
            // Stage 2 simplification (see docs/STAGE2_ARCHITECTURE.md).
            auto it = shard.lru.rbegin();
            while (it != shard.lru.rend() && it->dirtyState != EntryDirtyState::Clean) {
                ++it;
            }
            if (it == shard.lru.rend()) {
                break; // no evictable (Clean) entry left
            }
            auto forwardIt = std::next(it).base(); // convert reverse_iterator to iterator
            shard.currentBytes -= forwardIt->SizeBytes();
            shard.index.erase(forwardIt->key);
            shard.lru.erase(forwardIt);
            evictionCount_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // AUDITED BUG (Stage 2.5 hardening, fixed): this used to perform AT
    // MOST ONE opportunistic flush attempt per Put() call, regardless of
    // how far over budget the shard was or how many concurrent writers
    // were racing ahead of flush throughput. Empirically measured (see
    // Stage 2.5 hardening notes / CacheEngineTests.cpp's
    // BackpressureStressTests): under adversarial single-shard-skew
    // (shardCount effectively 1) with a slow backing store and heavy
    // concurrent write pressure, per-shard memory reached ~3.6x the
    // configured budget and was STILL SLOWLY CLIMBING after 30 seconds
    // of sustained load with the old one-flush-per-call mechanism —
    // i.e. `capacityBytes` was not a real safety limit under this
    // workload, only a soft, easily-defeated target. This directly
    // contradicts CacheEngineOptions::capacityBytes's documented
    // "budget" contract and the "no unbounded resource growth" resource
    // requirement.
    //
    // THE FIX: real proportional backpressure/throttling. A Put() call
    // that leaves its shard over budget now synchronously performs
    // REPEATED flush attempts (the same durable Flush() work
    // Flush()/FlushAll() already do — nothing new or less safe) until
    // EITHER the shard is back within its configured budget, OR no
    // further forward progress can be made right now (nothing
    // genuinely Dirty left to flush — everything remaining is Clean,
    // already handled by EvictCleanEntriesLocked, or FlushInProgress via
    // another thread's concurrent flush already in flight), OR a flush
    // attempt reports a genuine failure (backing store error — the
    // caller must not be blocked forever on a broken backing store).
    // This directly and deliberately SLOWS DOWN (throttles) writers to a
    // shard the backing store cannot keep up with — the standard,
    // correct meaning of "backpressure" — rather than silently letting
    // memory grow past the configured budget.
    //
    // NEVER discards or evicts dirty data: every flush performed here is
    // the exact same durable, journaled Flush() sequence used elsewhere;
    // if a flush genuinely fails, the entry simply stays Dirty (safely
    // durable in the journal) and this function returns without forcing
    // further, potentially-unbounded blocking.
    //
    // Progress is verified explicitly (shard.dirtyCount decreasing
    // between iterations) rather than assumed, specifically to avoid a
    // busy-spin: if a picked "least-recently-used Dirty" key turns out
    // to already be FlushInProgress-owned by a concurrent flush by the
    // time FlushKey() actually runs (a real, benign race — see
    // FlushKey's own FlushInProgress short-circuit), FlushKey() reports
    // Success() without doing any work and dirtyCount will not have
    // moved; this function detects that and stops immediately rather
    // than looping on a no-op.
    //
    // kMaxFlushAttemptsPerCall is a safety valve, not a correctness
    // requirement: it exists only to bound how long a single Put() call
    // can be blocked if a backing store is persistently failing (not
    // merely slow) in a way that keeps reporting a *different* key
    // failing each time (an unusual, adversarial pattern) — ordinary
    // sustained slowness is bounded instead by the "no progress -> stop"
    // check above, which triggers almost immediately once the backlog is
    // being serviced by other concurrent flushers.
    static constexpr int kMaxFlushAttemptsPerCall = 64;

    void RelieveCapacityPressureIfNeeded(Shard& shard) {
        for (int attempt = 0; attempt < kMaxFlushAttemptsPerCall; ++attempt) {
            std::string keyToFlush;
            std::size_t dirtyCountBefore = 0;
            {
                std::lock_guard<std::mutex> lock(shard.mutex);
                bool overBudget = shard.currentBytes > perShardCapacityBytes_ ||
                                   shard.index.size() > perShardMaxEntries_;
                if (!overBudget) return; // back within budget: done.

                dirtyCountBefore = shard.dirtyCount;

                for (auto it = shard.lru.rbegin(); it != shard.lru.rend(); ++it) {
                    if (it->dirtyState == EntryDirtyState::Dirty) {
                        keyToFlush = it->key;
                        break;
                    }
                }

                if (keyToFlush.empty()) {
                    // Nothing safely flushable right now (every
                    // remaining over-budget entry is Clean — handled by
                    // EvictCleanEntriesLocked below — or already
                    // FlushInProgress elsewhere). Cannot force further
                    // progress without violating "never discard dirty
                    // data"; let the in-flight flush(es) finish
                    // naturally and stop here for this call.
                    EvictCleanEntriesLocked(shard);
                    return;
                }
            }

            auto flushResult = FlushKey(keyToFlush, 0, /*checkVersion=*/false);

            std::size_t dirtyCountAfter = 0;
            {
                std::lock_guard<std::mutex> lock(shard.mutex);
                EvictCleanEntriesLocked(shard);
                dirtyCountAfter = shard.dirtyCount;
            }

            if (!flushResult) {
                // Genuine backing-store failure (NOT "already in
                // progress", which FlushKey reports as Success()) — the
                // data remains safely Dirty/durably journaled; do not
                // spin retrying the same failure inside this call. A
                // later Flush()/FlushAll()/periodic background
                // flush/recovery will retry it.
                if (logger_) {
                    logger_->Log(Logging::LogLevel::Warning, "CoreEngine",
                                 "RelieveCapacityPressureIfNeeded: flush attempt failed while over "
                                 "budget (" + flushResult.Err().message +
                                 "); shard may temporarily exceed its configured capacity.");
                }
                return;
            }

            if (dirtyCountAfter >= dirtyCountBefore) {
                // No forward progress this iteration (the targeted key
                // was concurrently claimed/completed/superseded by
                // another thread before our FlushKey() call did real
                // work) — stop rather than busy-spin; concurrent
                // activity is already making progress on this shard.
                return;
            }
            // Real progress was made (dirtyCount decreased); loop again
            // to check whether the shard is back within budget yet.
        }

        if (logger_) {
            logger_->Log(Logging::LogLevel::Warning, "CoreEngine",
                         "RelieveCapacityPressureIfNeeded: exhausted " +
                             std::to_string(kMaxFlushAttemptsPerCall) +
                             " flush attempts while still over budget; shard may temporarily "
                             "exceed its configured capacity until flush throughput catches up.");
        }
    }

    // Core flush implementation shared by Flush(), FlushAll(), and the
    // WriteThrough path in Put(). If `checkVersion` is true, only flushes
    // if the entry's current version still equals `expectedVersion`
    // (used by Put()'s WriteThrough path to flush precisely the write it
    // just made, not a newer one that raced ahead of it).
    Result<void> FlushKey(const std::string& key, std::uint64_t expectedVersion, bool checkVersion = true) {
        Shard& shard = ShardFor(key);

        std::vector<std::uint8_t> value;
        std::uint64_t version = 0;
        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            auto it = shard.index.find(key);
            if (it == shard.index.end()) {
                return Result<void>::Failure(
                    Error{ErrorCode::NotFound, "key not present in cache: " + key, 0});
            }
            if (checkVersion && it->second->version != expectedVersion) {
                return Result<void>::Success(); // superseded; nothing to do
            }
            if (it->second->dirtyState == EntryDirtyState::Clean) {
                return Result<void>::Success(); // already flushed; no-op
            }
            if (it->second->dirtyState == EntryDirtyState::FlushInProgress) {
                // Another flush of this exact key is already in flight;
                // let it own completion rather than doing redundant I/O.
                return Result<void>::Success();
            }

            value = it->second->value;
            version = it->second->version;
            it->second->dirtyState = EntryDirtyState::FlushInProgress;
        }

        CacheJournalRecord intentRecord;
        intentRecord.type = CacheRecordType::FlushIntent;
        intentRecord.key = key;
        intentRecord.entryVersion = version;
        auto intentResult = AppendJournalRecord(intentRecord);
        if (!intentResult) {
            RevertFlushInProgressLocked(shard, key, version);
            flushFailureCount_.fetch_add(1, std::memory_order_relaxed);
            return intentResult;
        }

        auto putResult = backingStore_->Put(key, value);
        if (!putResult) {
            RevertFlushInProgressLocked(shard, key, version);
            flushFailureCount_.fetch_add(1, std::memory_order_relaxed);
            return putResult;
        }

        CacheJournalRecord completeRecord;
        completeRecord.type = CacheRecordType::FlushComplete;
        completeRecord.key = key;
        completeRecord.entryVersion = version;
        auto completeResult = AppendJournalRecord(completeRecord);
        if (!completeResult) {
            // The backing store write DID land, but we could not durably
            // record that fact. Safer to leave the entry marked dirty
            // (it will be flushed again — backing-store Put is
            // idempotent/overwriting — rather than risk a
            // never-recorded, silently-lost completion).
            RevertFlushInProgressLocked(shard, key, version);
            flushFailureCount_.fetch_add(1, std::memory_order_relaxed);
            return completeResult;
        }

        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            auto it = shard.index.find(key);
            if (it != shard.index.end() && it->second->version == version &&
                it->second->dirtyState == EntryDirtyState::FlushInProgress) {
                std::size_t sizeBytes = it->second->SizeBytes();
                it->second->dirtyState = EntryDirtyState::Clean;
                shard.dirtyBytes -= sizeBytes;
                --shard.dirtyCount;
            }
            // If the entry is missing or has moved past this version,
            // a newer write/removal already superseded this flush's
            // target — correctly nothing to do.
        }

        flushSuccessCount_.fetch_add(1, std::memory_order_relaxed);
        return Result<void>::Success();
    }

    void RevertFlushInProgressLocked(Shard& shard, const std::string& key, std::uint64_t version) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto it = shard.index.find(key);
        if (it != shard.index.end() && it->second->version == version &&
            it->second->dirtyState == EntryDirtyState::FlushInProgress) {
            it->second->dirtyState = EntryDirtyState::Dirty; // retryable
        }
    }

    Result<void> InvalidateImpl(const std::string& key, bool force) {
        auto readyCheck = CheckReadyForWrite();
        if (!readyCheck) return readyCheck;

        // AUDITED BUG (fixed): same admission requirement as Put() —
        // see TryAdmitWrite()'s comment. Invalidate()/ForceInvalidate()
        // also durably append a journal record (the Invalidate record)
        // and then commit to shard state, so they are exactly as
        // exposed to the Shutdown()-truncation race as Put() is.
        if (!TryAdmitWrite()) {
            return Result<void>::Failure(Error{
                ErrorCode::ServiceStopping,
                "cache engine is shutting down and no longer accepts new writes", 0});
        }
        WriteAdmissionGuard admissionGuard(*this);

        Shard& shard = ShardFor(key);
        std::uint64_t entryVersion = 0;
        bool existedInCache = false;
        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            auto it = shard.index.find(key);
            if (it != shard.index.end()) {
                existedInCache = true;
                entryVersion = it->second->version;
                if (!force && it->second->dirtyState != EntryDirtyState::Clean) {
                    return Result<void>::Failure(Error{
                        ErrorCode::UnflushedDirtyData,
                        "refusing to invalidate '" + key +
                            "': it has unflushed dirty data (use ForceInvalidate to override)", 0});
                }
            }
        }
        (void)existedInCache;

        CacheJournalRecord record;
        record.type = CacheRecordType::Invalidate;
        record.key = key;
        record.entryVersion = entryVersion;
        auto journalResult = AppendJournalRecord(record);
        if (!journalResult) return journalResult;

        // Order matters: remove from the backing store BEFORE the cache,
        // so a failure here leaves the system in a safe, retryable state
        // (see file-level Invalidate() contract discussion in
        // ICacheEngine.h / docs/STAGE2_ARCHITECTURE.md "Invalidation
        // ordering").
        auto backingResult = backingStore_->Remove(key);
        if (!backingResult) {
            return backingResult;
        }

        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            auto it = shard.index.find(key);
            // AUDITED BUG (now fixed): the original code erased whatever
            // entry currently maps to `key` here, unconditionally. But
            // between the first locked snapshot above and this second
            // locked section, a concurrent Put() for the SAME key could
            // have committed a brand-new, newer Dirty entry (with a
            // different version) — and that unconditional erase would
            // silently destroy that newer write's live cache
            // representation, the exact "stale result overwrites/erases
            // a newer entry" class of bug this audit was scoped to catch
            // (here manifesting as an incorrect REMOVAL rather than an
            // incorrect stale VALUE, but the same root cause: a "commit"
            // phase acting on a linearization point that may no longer
            // be current). The fix: only erase if the entry's version
            // still matches exactly what was snapshotted/journaled above
            // — i.e. this invalidate only ever removes the SPECIFIC
            // entry instance it decided to remove, never a newer one
            // that happened to arrive at the same key afterward.
            if (it != shard.index.end() && it->second->version == entryVersion) {
                std::size_t sizeBytes = it->second->SizeBytes();
                if (it->second->dirtyState != EntryDirtyState::Clean) {
                    shard.dirtyBytes -= sizeBytes;
                    --shard.dirtyCount;
                }
                shard.currentBytes -= sizeBytes;
                shard.lru.erase(it->second);
                shard.index.erase(it);
            }
            // else: a newer write (or a concurrent read-fill that landed
            // after our snapshot) already changed this key; this
            // invalidate's job — removing the specific value it targeted
            // — is already moot, and leaving the newer state alone is
            // the correct outcome.

            // AUDITED BUG (now fixed): the fence must advance for EVERY
            // successful invalidate/removal that reaches this point —
            // including when the key had NO live cache entry at all
            // (existedInCache == false above, e.g. ForceInvalidate()
            // called on a key that was never read into the cache).
            // Originally the fence was only bumped inside the
            // `if (it != shard.index.end() && ...)` branch above, so an
            // invalidate of an absent-from-cache key was invisible to
            // Get()'s read-fill fence check. But a concurrent Get() miss
            // can legitimately be racing an authoritative
            // backingStore_->Remove() for that exact key (this is
            // precisely what just happened a few lines above, durably
            // and authoritatively) — that read-fill, once its unlocked
            // backing-store read completes with the now-stale
            // about-to-be-removed value, MUST be forced to re-check and
            // discard rather than resurrect it. Since an
            // Invalidate()/ForceInvalidate() call durably journals and
            // removes from the backing store REGARDLESS of prior cache
            // presence, it must ALWAYS be treated as a real mutation of
            // this shard's authoritative state for this key, whether or
            // not there happened to be an in-memory Entry to also erase.
            ++shard.mutationFence;
        }

        invalidationCount_.fetch_add(1, std::memory_order_relaxed);
        return Result<void>::Success();
    }

    CacheEngineOptions options_;
    std::shared_ptr<Storage::IBackingStore> backingStore_;
    std::shared_ptr<PowerResilience::IWriteAheadJournal> journal_;
    std::shared_ptr<Logging::ILogger> logger_;

    std::vector<Shard> shards_;
    std::uint64_t perShardCapacityBytes_{0};
    std::uint64_t perShardMaxEntries_{0};

    std::mutex journalMutex_;
    std::atomic<std::uint64_t> globalVersionCounter_{0};
    std::atomic<EngineLifecycle> lifecycle_{EngineLifecycle::NotReady};

    // AUDITED BUG (fixed): Shutdown() used to decide "is the journal safe
    // to truncate?" purely by snapshotting each shard's dirtyCount AFTER
    // calling FlushAll() and flipping lifecycle_ to Stopped — with NO
    // coordination against a Put()/Invalidate() call that had ALREADY
    // passed its CheckReadyForWrite() gate (observing lifecycle_ == Ready)
    // and durably appended its record to the journal, but had not yet
    // reached its own shard-lock "commit" step that increments
    // shard.dirtyCount. Concretely, this interleaving was possible:
    //   T1 (Put):      CheckReadyForWrite() sees Ready -> passes
    //   T1 (Put):      AppendJournalRecordWithNewVersion() durably
    //                  appends the record (journal now has it on disk)
    //   T2 (Shutdown): lifecycle_.compare_exchange(Ready -> Stopping)
    //   T2 (Shutdown): FlushAll() (T1's write isn't in any shard's LRU
    //                  yet, so FlushAll() cannot see or flush it)
    //   T2 (Shutdown): lifecycle_.store(Stopped)
    //   T2 (Shutdown): remainingDirty == 0 across all shards (T1 hasn't
    //                  incremented dirtyCount yet) -> journal_->Truncate()
    //                  WIPES T1's already-durably-appended record
    //   T1 (Put):      finally takes shard.mutex, inserts the entry as
    //                  Dirty in memory, and returns Success() to its
    //                  caller — who now believes the write is safe, but
    //                  its ONLY durable record was just destroyed and the
    //                  in-memory copy will vanish forever on the next
    //                  crash/restart (or even just process exit, since
    //                  Shutdown() already ran).
    // This is a genuine, real "acknowledged write can be silently lost"
    // bug — not merely a performance/log-noise issue like the similar-
    // looking existing "opportunistic ordering" comment on
    // StopBackgroundFlushThread() above addresses.
    //
    // The fix: an explicit "in-flight write-path admission" counter,
    // incremented (under admissionMutex_) by any Put()/InvalidateImpl()
    // call that has just passed CheckReadyForWrite() and is about to do
    // journal/shard work, and decremented when that call fully finishes
    // (including its shard-lock commit step). Shutdown() flips
    // lifecycle_ to Stopping FIRST (so CheckReadyForWrite() rejects any
    // brand-new callers immediately, exactly as before), THEN calls
    // DrainInFlightWrites() to block until every writer that was already
    // admitted before the flip has fully finished its commit — only
    // after that is it safe to compute "how much is left dirty" and
    // decide whether truncation is safe. No new writer can be admitted
    // after the flip (CheckReadyForWrite() will refuse), so the count
    // can only monotonically decrease to zero.
    std::mutex admissionMutex_;
    std::condition_variable admissionDrainedCv_;
    std::uint64_t inFlightWriteCount_{0};

    // Set by TryCompactJournalIfFullyClean() (Priority 2.7 — journal
    // growth/compaction, see that method's full comment) to briefly
    // pause NEW write admissions while it takes a race-free "is
    // everything currently clean" snapshot. Distinct from lifecycle_
    // reaching a non-Ready state (Shutdown()'s mechanism): compaction
    // does not stop the engine, it only pauses admission for the brief,
    // no-I/O duration of the snapshot+truncate.
    bool compactionPaused_{false};

    // Call immediately after CheckReadyForWrite() succeeds, before any
    // journal/shard mutation, from every write-path entry point (Put(),
    // InvalidateImpl()). Returns false (and does NOT admit) if shutdown
    // began concurrently between the caller's own CheckReadyForWrite()
    // and this call — closing the exact race window described above,
    // since admission and the Ready-check are then effectively atomic
    // with respect to Shutdown()'s Stopping transition.
    [[nodiscard]] bool TryAdmitWrite() {
        std::unique_lock<std::mutex> lock(admissionMutex_);
        // Wait out a brief in-progress journal compaction pause (see
        // TryCompactJournalIfFullyClean()) — expected to resolve almost
        // immediately, since no I/O happens while paused other than the
        // truncate itself.
        admissionDrainedCv_.wait(lock, [this]() { return !compactionPaused_; });
        if (lifecycle_.load() != EngineLifecycle::Ready) {
            return false;
        }
        ++inFlightWriteCount_;
        return true;
    }

    void ReleaseWriteAdmission() {
        std::lock_guard<std::mutex> lock(admissionMutex_);
        if (inFlightWriteCount_ > 0) {
            --inFlightWriteCount_;
        }
        if (inFlightWriteCount_ == 0) {
            admissionDrainedCv_.notify_all();
        }
    }

    // Blocks until every write admitted before this call was invoked has
    // released. Safe to call only AFTER lifecycle_ has already been
    // moved off Ready (so TryAdmitWrite() can no longer admit anyone
    // new) — otherwise this could wait forever under sustained write
    // load. No arbitrary timeout: a bounded/best-effort wait here would
    // reintroduce exactly the class of "silently proceed as if drained"
    // bug this mechanism exists to close; every in-flight writer here is
    // by construction already past its journal append and only has a
    // brief, bounded (mutex-guarded, no I/O) shard-commit step left.
    void DrainInFlightWrites() {
        std::unique_lock<std::mutex> lock(admissionMutex_);
        admissionDrainedCv_.wait(lock, [this]() { return inFlightWriteCount_ == 0; });
    }

    // -----------------------------------------------------------------
    // Journal growth / compaction (Priority 2.7).
    //
    // AUDITED GAP (fixed): prior to this, the ONLY place the journal was
    // ever truncated was Shutdown() — meaning a long-running service
    // that never restarts would have its write-ahead journal grow
    // without bound for its entire uptime, even though every record in
    // it had long since been durably applied to the backing store (via
    // Flush()/FlushAll()) and was therefore pure dead weight. This is a
    // real, unbounded-resource-growth bug for exactly the kind of
    // always-on caching service this project targets — see Priority 8
    // ("low resource operation": "no busy loops/polling... bounded
    // journal/temp buffers").
    //
    // The fix: TryCompactJournalIfFullyClean(), an OPPORTUNISTIC,
    // conservative compaction that runs the exact same safety check
    // Shutdown() already uses (and this project's tests already trust —
    // see CacheEngineTests.cpp's Shutdown_FlushesDirtyDataAndTruncatesJournal),
    // just outside the shutdown path: truncate the journal ONLY when
    // EVERY shard reports zero dirty entries, i.e. the backing store and
    // the in-memory cache are already in 1:1 agreement and the journal
    // holds nothing that is not ALSO safely durable elsewhere. This is
    // called opportunistically after every successful FlushAll() (both
    // the manual API and the periodic background flush thread's timer-
    // driven call), which is exactly the moment such an all-clean state
    // is most likely to actually hold.
    //
    // Concurrency safety: the same write-admission counter used to fix
    // the Shutdown()/Put() race (see TryAdmitWrite()'s comment above) is
    // reused here via a lighter-weight "pause admission briefly, take a
    // clean snapshot, resume" pattern (compactionPaused_) rather than
    // the heavier "stop admitting forever" used by real shutdown — a
    // running engine must keep accepting new writes; compaction only
    // needs a momentary, bounded (no I/O other than the truncate itself)
    // pause to get a race-free "is everything currently clean" answer,
    // then immediately resumes. NEVER discards journal content before
    // the corresponding cache state is confirmed durable elsewhere,
    // matching the audit's explicit requirement.
    //
    // Deliberately conservative, not a general log-structured compactor:
    // this project's journal is a simple append-only log, not indexed by
    // key, so partial/selective compaction (keeping only the "still
    // relevant" subset of records) is not implemented — only the
    // all-or-nothing "everything is clean, so the whole journal is dead
    // weight" case is handled. See docs/STAGE2_ARCHITECTURE.md "Journal
    // growth / compaction" for the honest limitation this implies under
    // a workload with even one perpetually-dirty key (compaction never
    // fires, and the journal keeps every historical record) and the
    // long-running growth test in CacheEngineTests.cpp that exercises
    // this.
    void TryCompactJournalIfFullyClean() {
        // AUDITED BUG (Stage 2.5 hardening, fixed): this function used
        // to unconditionally pause write admission (blocking every
        // concurrent Put()/InvalidateImpl() attempt) and perform a real,
        // blocking journal_->Truncate() call — including a genuine
        // fsync()-class durability syscall via
        // WriteAheadJournal::Truncate()'s FlushDurable() — on EVERY
        // single successful FlushAll() call, even when the journal was
        // already empty (nothing to compact). Since FlushAll() is called
        // by the periodic background flush thread AND can legitimately
        // be called back-to-back by application code, and since this
        // work was done under a real mutex (journalMutex_) shared with
        // every Put()/Invalidate()/Flush() journal append, a workload
        // with several threads calling FlushAll() in a tight loop caused
        // SEVERE, near-total lock contention/livelock: independently
        // discovered via CacheEngineCompactionAuditTests.cpp's
        // ConcurrentPutInvalidateFlushAndCompaction_* stress test, which
        // hung for over 60 seconds (confirmed via gdb thread dump: all
        // threads were legitimately executing — repeatedly re-entering
        // TryCompactJournalIfFullyClean()/fsync() — rather than deadlocked,
        // making this a livelock/starvation bug, not a deadlock, but
        // just as severe a production hazard for any workload that
        // calls FlushAll() frequently (e.g. a short
        // flushIntervalSeconds, or an application explicitly flushing
        // after every write for a stronger durability posture).
        //
        // THE FIX: check journal_->RecordCount() FIRST, before pausing
        // admission or touching journalMutex_ at all. RecordCount() is a
        // cheap, lock-free (from CacheEngine's perspective — it is a
        // plain in-memory counter read on the journal object itself)
        // check; if the journal is already empty, there is provably
        // nothing to compact and the entire admission-pause+Truncate()
        // sequence is skipped. This preserves ALL existing correctness
        // guarantees (never discards dirty data; behaves identically
        // whenever there genuinely IS something to compact) while
        // eliminating the wasted, contention-heavy work in the
        // overwhelmingly common case where the journal is already empty
        // between two nearly-back-to-back FlushAll() calls.
        //
        // AUDITED BUG (Stage 2.5 hardening, fixed): this early-exit
        // check originally called journal_->RecordCount() WITHOUT
        // holding journalMutex_ — a genuine data race (confirmed by
        // ThreadSanitizer: concurrent Put()/FlushKey() calls write to
        // the journal's internal recordCount_ field via Append() WHILE
        // HOLDING journalMutex_, so reading it here without that same
        // lock is undefined behavior per the C++ memory model, not
        // merely "reading possibly-stale data"). IWriteAheadJournal
        // provides no internal thread-safety of its own — see this
        // file's own header comment ("PowerResilience::IWriteAheadJournal
        // ... is not documented or implemented as thread-safe") — so
        // EVERY access to `journal_`, including a read-only
        // RecordCount() call, must go through journalMutex_, with no
        // exception for "it's just reading a counter." Fixed by
        // acquiring journalMutex_ for this check too. This does
        // reintroduce SOME lock acquisition on the fast path (unlike a
        // true lock-free counter would), but it is a single, uncontended
        // mutex lock/unlock with no I/O — negligible compared to the
        // real fsync-class Truncate() work this early-exit is skipping
        // in the common case, and correctness must never be traded for
        // this specific optimization's speed.
        {
            std::lock_guard<std::mutex> journalLock(journalMutex_);
            if (journal_->RecordCount() == 0) {
                return;
            }
        }

        // Pause new-write admission briefly so the "everything is
        // clean" snapshot below cannot be invalidated by a write that
        // starts concurrently — mirrors Shutdown()'s reasoning but
        // without ever transitioning lifecycle_ (the engine remains
        // fully Ready and serving reads/writes throughout; only brand
        // new admission attempts momentarily wait).
        {
            std::lock_guard<std::mutex> lock(admissionMutex_);
            compactionPaused_ = true;
        }

        // Wait for any writer that was ALREADY admitted before the pause
        // took effect to finish committing — same reasoning as
        // DrainInFlightWrites(), reused directly since the invariant
        // ("no new admissions once paused, so the count can only
        // decrease") is identical.
        DrainInFlightWrites();

        std::uint64_t remainingDirty = 0;
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            remainingDirty += shard.dirtyCount;
        }

        if (remainingDirty == 0) {
            std::lock_guard<std::mutex> journalLock(journalMutex_);
            // Re-check RecordCount() under journalMutex_: between the
            // lock-free check above and reaching here, another thread's
            // own TryCompactJournalIfFullyClean() call may have already
            // compacted the journal to zero (a real, common race under
            // concurrent FlushAll() calls) — calling Truncate() again
            // would be harmless (Truncate() on an already-empty journal
            // is a safe no-op — see WriteAheadJournal.cpp) but would
            // reintroduce exactly the redundant-fsync waste this fix
            // exists to eliminate.
            if (journal_->RecordCount() == 0) {
                std::lock_guard<std::mutex> lock(admissionMutex_);
                compactionPaused_ = false;
                admissionDrainedCv_.notify_all();
                return;
            }
            auto truncateResult = journal_->Truncate();
            if (!truncateResult && logger_) {
                logger_->Log(Logging::LogLevel::Warning, "CoreEngine",
                             "Opportunistic journal compaction: truncation failed (" +
                                 truncateResult.Err().message +
                                 "); journal will simply keep growing until the next successful "
                                 "attempt or a clean Shutdown().");
            } else if (truncateResult && logger_) {
                logger_->Log(Logging::LogLevel::Info, "CoreEngine",
                             "Opportunistic journal compaction: journal truncated (all entries "
                             "were confirmed Clean / already durable in the backing store).");
            }
        }
        // If remainingDirty > 0: correctly do nothing. Some entries were
        // still dirty at the moment of this check (either FlushAll()
        // partially failed, or a new write landed between FlushAll()
        // finishing and this check running) — the journal still holds
        // their only durable record and must not be touched. This is
        // not a failure, just "not compactable yet"; the next
        // opportunistic call (after the next successful FlushAll()) will
        // re-check.

        // Resume admission and wake anyone waiting in TryAdmitWrite().
        {
            std::lock_guard<std::mutex> lock(admissionMutex_);
            compactionPaused_ = false;
        }
        admissionDrainedCv_.notify_all();
    }

    // RAII helper so every write-path function releases its admission on
    // every exit path (normal return, early return, or an exception —
    // though this codebase does not throw across these boundaries,
    // matching the pattern used elsewhere for robustness) without
    // needing a manual ReleaseWriteAdmission() call at each return
    // statement. Constructed only after TryAdmitWrite() returns true.
    class WriteAdmissionGuard {
    public:
        explicit WriteAdmissionGuard(CacheEngine& engine) : engine_(engine) {}
        ~WriteAdmissionGuard() { engine_.ReleaseWriteAdmission(); }
        WriteAdmissionGuard(const WriteAdmissionGuard&) = delete;
        WriteAdmissionGuard& operator=(const WriteAdmissionGuard&) = delete;
    private:
        CacheEngine& engine_;
    };

    std::atomic<std::uint64_t> hitCount_{0};
    std::atomic<std::uint64_t> missCount_{0};
    std::atomic<std::uint64_t> insertCount_{0};
    std::atomic<std::uint64_t> updateCount_{0};
    std::atomic<std::uint64_t> invalidationCount_{0};
    std::atomic<std::uint64_t> evictionCount_{0};
    std::atomic<std::uint64_t> flushSuccessCount_{0};
    std::atomic<std::uint64_t> flushFailureCount_{0};

    // ---------------------------------------------------------------
    // Periodic background flush (Stage 2B).
    //
    // Only started when CacheEngineOptions::flushPolicy ==
    // FlushPolicyKind::PeriodicBackground, and only once, from
    // MarkRecoveryComplete() (never before recovery has completed, so the
    // background thread can never race with journal replay). Uses a
    // std::condition_variable rather than a plain sleep so
    // StopBackgroundFlushThread() can wake it immediately for a prompt,
    // bounded shutdown instead of waiting out up to a full
    // flushIntervalSeconds tick.
    // ---------------------------------------------------------------
    std::thread backgroundFlushThread_;
    std::mutex backgroundFlushMutex_;
    std::condition_variable backgroundFlushCv_;
    bool backgroundFlushStopRequested_{false};

    void StartBackgroundFlushThreadIfConfigured() {
        if (options_.flushPolicy != FlushPolicyKind::PeriodicBackground) return;
        if (backgroundFlushThread_.joinable()) return; // already running; do not start twice

        std::uint32_t intervalSeconds = std::max<std::uint32_t>(1, options_.flushIntervalSeconds);
        backgroundFlushStopRequested_ = false;

        backgroundFlushThread_ = std::thread([this, intervalSeconds]() {
            std::unique_lock<std::mutex> lock(backgroundFlushMutex_);
            while (!backgroundFlushStopRequested_) {
                // Wait up to intervalSeconds, but wake immediately if
                // StopBackgroundFlushThread() signals us — this is what
                // makes shutdown prompt rather than bounded only by the
                // configured interval.
                backgroundFlushCv_.wait_for(
                    lock, std::chrono::seconds(intervalSeconds),
                    [this]() { return backgroundFlushStopRequested_; });
                if (backgroundFlushStopRequested_) break;

                // Run the actual flush WITHOUT holding
                // backgroundFlushMutex_: FlushAll() performs real I/O
                // (journal + backing store) and must never be run while
                // holding a lock that StopBackgroundFlushThread() also
                // needs, or shutdown could block behind a long-running
                // flush unnecessarily. This matches this file's existing
                // "never hold a lock across I/O" discipline described in
                // the file-header CONCURRENCY MODEL comment.
                lock.unlock();
                // Check engine lifecycle readiness before invoking FlushAll().
                // Only flush if the engine is currently in EngineLifecycle::Ready state.
                auto lifecycleNow = lifecycle_.load();
                if (lifecycleNow == EngineLifecycle::Ready) {
                    auto flushResult = FlushAll();
                    if (!flushResult && logger_) {
                        logger_->Log(Logging::LogLevel::Warning, "CoreEngine",
                                     "Periodic background flush did not fully succeed (data remains "
                                     "safely journaled and will be recovered on next startup): " +
                                         flushResult.Err().message);
                    }
                }
                lock.lock();
            }
        });

        if (logger_) {
            logger_->Log(Logging::LogLevel::Info, "CoreEngine",
                         "Periodic background flush thread started (interval=" +
                             std::to_string(intervalSeconds) + "s).");
        }
    }

    void StopBackgroundFlushThread() {
        if (!backgroundFlushThread_.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(backgroundFlushMutex_);
            backgroundFlushStopRequested_ = true;
        }
        backgroundFlushCv_.notify_all();
        backgroundFlushThread_.join();
    }
};

} // namespace

Result<std::unique_ptr<ICacheEngine>> CreateCacheEngine(
    const CacheEngineOptions& options,
    std::shared_ptr<Storage::IBackingStore> backingStore,
    std::shared_ptr<PowerResilience::IWriteAheadJournal> journal,
    std::shared_ptr<Logging::ILogger> logger) {
    if (!backingStore) {
        return Result<std::unique_ptr<ICacheEngine>>::Failure(
            Error{ErrorCode::InvalidArgument, "backingStore must not be null", 0});
    }
    if (!journal) {
        return Result<std::unique_ptr<ICacheEngine>>::Failure(
            Error{ErrorCode::InvalidArgument, "journal must not be null", 0});
    }
    if (options.shardCount == 0 || (options.shardCount & (options.shardCount - 1)) != 0) {
        return Result<std::unique_ptr<ICacheEngine>>::Failure(
            Error{ErrorCode::InvalidArgument, "shardCount must be a power of two >= 1", 0});
    }
    if (options.capacityBytes == 0) {
        return Result<std::unique_ptr<ICacheEngine>>::Failure(
            Error{ErrorCode::InvalidArgument, "capacityBytes must be > 0", 0});
    }
    if (options.maxEntryCount == 0) {
        return Result<std::unique_ptr<ICacheEngine>>::Failure(
            Error{ErrorCode::InvalidArgument, "maxEntryCount must be > 0", 0});
    }

    return Result<std::unique_ptr<ICacheEngine>>::Success(
        std::make_unique<CacheEngine>(options, std::move(backingStore), std::move(journal),
                                       std::move(logger)));
}

} // namespace QuantumCache::CoreEngine
