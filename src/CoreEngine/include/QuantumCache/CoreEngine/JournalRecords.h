#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Stage 2: gives QuantumCache::PowerResilience::IWriteAheadJournal's
// generic, opaque-payload records actual CACHE semantics for the first
// time. Stage 1 deliberately left the journal payload format undefined
// ("infrastructure the future cache engine will use"); this header is
// that future arriving. It stays inside CoreEngine (not PowerResilience)
// because PowerResilience must remain agnostic of what it is journaling —
// exactly as documented in IWriteAheadJournal.h.
//
// Every record is versioned (`kCacheRecordFormatVersion`) and
// self-describing via `CacheRecordType`, independent of the journal
// frame's own CRC-32 (IWriteAheadJournal already guarantees a record
// handed to Replay() was not torn/corrupted at the frame level; this
// layer defines what the bytes *inside* a valid frame mean).
namespace QuantumCache::CoreEngine {

constexpr std::uint32_t kCacheRecordFormatVersion = 1;

// Every cache-semantic operation that can appear in the write-ahead
// journal. Deliberately narrow and matched 1:1 to what CacheEngine (this
// stage) actually performs — no speculative "future" record types.
enum class CacheRecordType : std::uint32_t {
    // A key's value was inserted or updated in the cache and is now
    // considered dirty (not yet reflected in the backing store).
    Upsert = 1,

    // A previously-dirty entry has begun being flushed to the backing
    // store. Written BEFORE the backing-store write is issued, so replay
    // can tell "we attempted a flush but don't know if it landed" apart
    // from "we never tried."
    FlushIntent = 2,

    // The flush named by a prior FlushIntent record completed and was
    // confirmed durable at the backing store. After this record, the
    // entry is clean and the corresponding Upsert/FlushIntent records for
    // that key (up to and including this one) are logically obsolete.
    FlushComplete = 3,

    // A key was explicitly removed/invalidated from the cache.
    Invalidate = 4,
};

[[nodiscard]] inline const char* ToString(CacheRecordType type) noexcept {
    switch (type) {
        case CacheRecordType::Upsert: return "Upsert";
        case CacheRecordType::FlushIntent: return "FlushIntent";
        case CacheRecordType::FlushComplete: return "FlushComplete";
        case CacheRecordType::Invalidate: return "Invalidate";
    }
    return "InvalidCacheRecordType";
}

// Decoded form of one journal payload. `key`/`value` are only meaningful
// for the record types that use them (see field comments); encoding always
// writes a canonical, versioned binary layout (see JournalRecordCodec.cpp)
// so a future format change can be detected via `formatVersion` rather
// than silently misparsed.
struct CacheJournalRecord {
    std::uint32_t formatVersion{kCacheRecordFormatVersion};
    CacheRecordType type{CacheRecordType::Upsert};

    // Cache key this record concerns. Present on every record type.
    std::string key;

    // Value bytes. Only populated (and only meaningful) for Upsert.
    std::vector<std::uint8_t> value;

    // Monotonically increasing per-key version, assigned by the cache
    // engine at the moment of insertion/update. Lets replay distinguish
    // "this Upsert is newer than that one for the same key" without
    // relying on wall-clock time, and lets FlushIntent/FlushComplete
    // unambiguously refer to a specific Upsert even if the key was
    // updated again before the flush finished.
    std::uint64_t entryVersion{0};
};

} // namespace QuantumCache::CoreEngine
