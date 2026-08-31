#pragma once
#include "QuantumCache/Common/Result.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace QuantumCache::Storage {

// -----------------------------------------------------------------------
// STAGE 2 SCOPE NOTE — read this before assuming what this class is.
// -----------------------------------------------------------------------
// IBackingStore models the slow, authoritative storage that QuantumCache
// caches in front of (conceptually: the HDD/network volume the user is
// accelerating). It is explicitly NOT:
//   - an SSD cache (there is no L2/SSD tier in Stage 2 — see project scope
//     notes in docs/STAGE2_ARCHITECTURE.md);
//   - a kernel-mode block/volume filter driver (Stage 2 has none);
//   - a simulation or mockup — FileBackingStore (the only implementation
//     in Stage 2) is a real, working, crash-consistent key/value store
//     built on top of IFile, and every byte it reports as stored has
//     actually been written and durably flushed to a real file.
//
// The distinction that matters: CacheEngine's in-memory shards ARE a real
// RAM cache; IBackingStore is the real (if simplified, file-based rather
// than raw-volume/driver-based) persisted store that RAM cache is
// accelerating access to. Nothing here pretends the RAM cache itself is
// durable, and nothing here pretends this file-based store is an SSD.
class IBackingStore {
public:
    virtual ~IBackingStore() = default;

    // Returns ErrorCode::NotFound if the key has never been stored (or was
    // removed), which is a normal, expected outcome — not a failure of the
    // store itself. Any other failure (IoError, CorruptData) is a genuine
    // problem with the backing store.
    [[nodiscard]] virtual Common::Result<std::vector<std::uint8_t>> Get(
        const std::string& key) = 0;

    struct BatchItem {
        std::string key;
        std::vector<std::uint8_t> value;
    };

    // Durably persists `value` under `key`, appending to the underlying
    // log and requiring a caller-visible success only once the write is
    // confirmed on stable storage (FlushDurable is called internally
    // before this returns Ok) — consistent with the rest of this
    // project's "never fake durability" rule.
    [[nodiscard]] virtual Common::Result<void> Put(
        const std::string& key, const std::vector<std::uint8_t>& value) = 0;

    // Durably persists a batch of key/value pairs in one batch append
    // and performs a single FlushDurable call. Default implementation
    // loops over Put for backward compatibility with decorators.
    [[nodiscard]] virtual Common::Result<void> PutBatch(
        const std::vector<BatchItem>& items) {
        for (const auto& item : items) {
            auto res = Put(item.key, item.value);
            if (!res) return res;
        }
        return Common::Result<void>::Success();
    }

    [[nodiscard]] virtual Common::Result<void> Remove(const std::string& key) = 0;

    [[nodiscard]] virtual bool Contains(const std::string& key) = 0;

    [[nodiscard]] virtual std::size_t EntryCount() const noexcept = 0;
};

// Opens (or creates) a file-backed key/value store at `dataFilePath`. Uses
// Storage::OpenFile/IFile internally — the same durable-file abstraction
// PowerResilience uses for the session marker and journal — so this
// inherits real crash-consistency behavior (append-only records with a
// CRC-32 trailer; a torn tail record from a mid-write power cut is
// detected and discarded during the startup scan, exactly like the
// write-ahead journal's own Replay()).
[[nodiscard]] Common::Result<std::unique_ptr<IBackingStore>> OpenFileBackingStore(
    const std::wstring& dataFilePath);

} // namespace QuantumCache::Storage
