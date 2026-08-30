#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace QuantumCache::CoreEngine {

// -----------------------------------------------------------------------
// Cache entry identity, metadata, and durability state (Stage 2).
// -----------------------------------------------------------------------

// Where a specific cache entry currently sits with respect to the backing
// store. This is the concrete "dirty-data/deferred-write state model"
// required for Stage 2: every entry is in exactly one of these states at
// any time, and every transition between them is made under the owning
// shard's lock (see CacheEngine.cpp) so a reader never observes a
// half-updated state.
enum class EntryDirtyState : std::uint32_t {
    // The entry's value is identical to what is durably stored in the
    // backing store (or was just read FROM the backing store and inserted
    // as clean). Safe to evict at any time.
    Clean = 0,

    // The entry's value has been journaled (durably, via
    // IWriteAheadJournal::Append + FlushDurable) but has NOT yet been
    // written to the backing store. Must never be evicted from the cache
    // while dirty, or the backing store would appear authoritative while
    // actually being stale.
    Dirty = 1,

    // A flush of this entry to the backing store is currently underway
    // (FlushIntent has been journaled; the backing-store write has been
    // issued but not yet confirmed). Distinguishing this from plain Dirty
    // lets recovery tell "we attempted this" apart from "we never tried",
    // which matters because a torn write to the backing store itself
    // means the value must be treated as still not-yet-durable there.
    FlushInProgress = 2,
};

[[nodiscard]] inline const char* ToString(EntryDirtyState state) noexcept {
    switch (state) {
        case EntryDirtyState::Clean: return "Clean";
        case EntryDirtyState::Dirty: return "Dirty";
        case EntryDirtyState::FlushInProgress: return "FlushInProgress";
    }
    return "InvalidEntryDirtyState";
}

// Identity + metadata for one cache entry, WITHOUT its data payload.
// Returned by diagnostic/inspection paths (and usable by IPC status
// reporting) so callers can reason about what is cached without forcing a
// copy of potentially large value bytes.
struct CacheEntryInfo {
    std::string key;
    std::uint64_t version{0};
    std::size_t sizeBytes{0};
    EntryDirtyState dirtyState{EntryDirtyState::Clean};
};

// -----------------------------------------------------------------------
// Engine-wide configuration and policy (Stage 2).
// -----------------------------------------------------------------------

// Only one policy is implemented in Stage 2 (LRU), but this is an enum
// (not a bool) so Configuration validation has a real, extensible set of
// named values to check against rather than an implicit "the only
// option", and so an unimplemented policy is REJECTED by validation
// rather than silently falling back to LRU.
enum class EvictionPolicyKind : std::uint32_t {
    LeastRecentlyUsed = 0,
};

[[nodiscard]] inline const char* ToString(EvictionPolicyKind policy) noexcept {
    switch (policy) {
        case EvictionPolicyKind::LeastRecentlyUsed: return "LeastRecentlyUsed";
    }
    return "InvalidEvictionPolicyKind";
}

// WriteThrough: Put() journals AND synchronously writes the backing store
//   before returning success; the entry is clean the moment Put() returns.
//   Slower, but there is never a window where cache and backing store
//   disagree beyond the call itself.
// WriteBackDeferred: Put() only journals (durably) before returning
//   success; the backing-store write happens later via Flush()/FlushAll()
//   (explicit or periodic, see FlushPolicyKind). Faster, at the cost of a
//   longer window where the backing store is stale — but never at the
//   cost of losing the write, because the journal already has it durably.
enum class WritePolicyKind : std::uint32_t {
    WriteThrough = 0,
    WriteBackDeferred = 1,
};

[[nodiscard]] inline const char* ToString(WritePolicyKind policy) noexcept {
    switch (policy) {
        case WritePolicyKind::WriteThrough: return "WriteThrough";
        case WritePolicyKind::WriteBackDeferred: return "WriteBackDeferred";
    }
    return "InvalidWritePolicyKind";
}

// Manual: dirty entries are only flushed when Flush()/FlushAll() is
//   called explicitly (including the flush CacheEngine::Shutdown()
//   performs before allowing a clean shutdown to be recorded).
// PeriodicBackground: in addition to Manual's triggers, a background
//   thread calls FlushAll() every `flushIntervalSeconds`.
enum class FlushPolicyKind : std::uint32_t {
    Manual = 0,
    PeriodicBackground = 1,
};

[[nodiscard]] inline const char* ToString(FlushPolicyKind policy) noexcept {
    switch (policy) {
        case FlushPolicyKind::Manual: return "Manual";
        case FlushPolicyKind::PeriodicBackground: return "PeriodicBackground";
    }
    return "InvalidFlushPolicyKind";
}

struct CacheEngineOptions {
    bool enabled{true};

    // Approximate total memory budget across all shards, in bytes,
    // counting each entry's key length + value length (see
    // CacheEngine.cpp EstimateEntrySize). "Approximate" because capacity
    // is enforced per-shard (capacityBytes / shardCount each) rather than
    // via one globally-coordinated counter — a deliberate, documented
    // concurrency/simplicity tradeoff (see docs/STAGE2_ARCHITECTURE.md).
    std::uint64_t capacityBytes{256ull * 1024 * 1024};

    // Hard cap on total entry count, enforced the same per-shard way as
    // capacityBytes. Exists independently of capacityBytes because a
    // workload with many tiny keys could otherwise stay under the byte
    // budget while still growing unboundedly in map/index overhead.
    std::uint64_t maxEntryCount{100000};

    EvictionPolicyKind evictionPolicy{EvictionPolicyKind::LeastRecentlyUsed};
    WritePolicyKind writePolicy{WritePolicyKind::WriteBackDeferred};
    FlushPolicyKind flushPolicy{FlushPolicyKind::Manual};

    // Only meaningful when flushPolicy == PeriodicBackground.
    std::uint32_t flushIntervalSeconds{30};

    // Number of independent lock-protected shards the keyspace is hashed
    // across. More shards = more concurrency, at the cost of splitting
    // capacityBytes/maxEntryCount that many ways (see above). Must be a
    // power of two for the fast masking hash-to-shard mapping used in
    // CacheEngine.cpp.
    std::uint32_t shardCount{16};
};

// -----------------------------------------------------------------------
// Observability (Stage 2).
// -----------------------------------------------------------------------

// Every counter here is incremented ONLY at the point the real operation
// it names actually happens (see CacheEngine.cpp) — never synthesized,
// estimated, or incremented speculatively. Read with GetStatistics(),
// which takes a consistent-enough snapshot (each counter is an
// individual atomic load; counters are not globally locked together,
// so under concurrent load two counters read a few nanoseconds apart
// could reflect slightly different points in time — acceptable for
// monitoring/diagnostics, not used for correctness decisions).
struct CacheStatistics {
    std::uint64_t hitCount{0};
    std::uint64_t missCount{0};
    std::uint64_t insertCount{0};
    std::uint64_t updateCount{0};
    std::uint64_t invalidationCount{0};
    std::uint64_t evictionCount{0};
    std::uint64_t flushSuccessCount{0};
    std::uint64_t flushFailureCount{0};

    std::uint64_t currentEntryCount{0};
    std::uint64_t currentMemoryBytes{0};
    std::uint64_t dirtyEntryCount{0};
    std::uint64_t dirtyBytes{0};
};

} // namespace QuantumCache::CoreEngine
