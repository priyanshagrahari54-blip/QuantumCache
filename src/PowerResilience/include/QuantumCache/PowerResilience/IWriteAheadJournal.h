#pragma once
#include "QuantumCache/Common/Result.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace QuantumCache::Storage { class IFile; }

namespace QuantumCache::PowerResilience {

// A generic, append-only, crash-consistent record log. This is
// infrastructure the future cache/deferred-write engine will use to make
// its own operations crash-safe — Stage 1 defines and implements the
// journal mechanics (append, durable commit, replay, truncate) but
// deliberately carries NO knowledge of cache semantics: record payloads
// are opaque byte blobs to this component. This is what "do not implement
// the complete cache engine yet, but include the recovery interfaces"
// means concretely: the durability plumbing exists and is real; the
// meaning of what gets journaled does not exist yet.
struct JournalRecord {
    std::uint64_t sequenceNumber{0};
    std::vector<std::uint8_t> payload;
};

enum class ReplayAction {
    Continue,        // Record was valid; keep replaying.
    StopReplaySuccess, // Deliberately stop early; treat as successful replay.
};

using ReplayCallback = std::function<Common::Result<ReplayAction>(const JournalRecord&)>;

class IWriteAheadJournal {
public:
    virtual ~IWriteAheadJournal() = default;

    // Appends one record and durably flushes it (fsync/FlushFileBuffers)
    // before returning success — the record is guaranteed to survive a
    // power loss occurring immediately after this call returns Ok.
    [[nodiscard]] virtual Common::Result<std::uint64_t> Append(
        const std::vector<std::uint8_t>& payload) = 0;

    // Appends one record without calling FlushDurable(), allowing caller to
    // batch multiple appends before issuing a single FlushDurable().
    [[nodiscard]] virtual Common::Result<std::uint64_t> AppendNoFlush(
        const std::vector<std::uint8_t>& payload) = 0;

    // Durably flushes any pending journal records to stable media.
    [[nodiscard]] virtual Common::Result<void> FlushDurable() = 0;

    // Replays every valid, fully-committed record in sequence order,
    // invoking `callback` for each. Stops (without error) at the first
    // record that fails its integrity check (CRC mismatch / truncated
    // write), since that is the expected shape of a torn write caused by
    // a power cut mid-append: everything before the tear is valid and
    // everything from the tear onward must be discarded, never
    // interpreted.
    [[nodiscard]] virtual Common::Result<void> Replay(const ReplayCallback& callback) = 0;

    // Discards all records (used once the caller has durably applied
    // everything the journal recorded and no longer needs it, e.g. after
    // the real cache engine folds journaled operations into steady-state
    // storage). Stage 1 exposes this as part of the contract but the only
    // caller in Stage 1 is the RecoveryManager test/demo path.
    [[nodiscard]] virtual Common::Result<void> Truncate() = 0;

    [[nodiscard]] virtual std::size_t RecordCount() const noexcept = 0;
};

// Factory: wraps an already-opened durable file as a write-ahead journal.
[[nodiscard]] Common::Result<std::unique_ptr<IWriteAheadJournal>> CreateWriteAheadJournal(
    std::unique_ptr<Storage::IFile> file);

} // namespace QuantumCache::PowerResilience
