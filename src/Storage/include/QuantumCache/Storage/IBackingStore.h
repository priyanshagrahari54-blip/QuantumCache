#pragma once
#include "QuantumCache/Common/Result.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace QuantumCache::Storage {

struct BackingStoreRecord {
    std::string key;
    std::vector<std::uint8_t> value;
    std::uint64_t version{0};
    bool tombstone{false};
};

// -----------------------------------------------------------------------
// STAGE 2 SCOPE NOTE — read this before assuming what this class is.
// -----------------------------------------------------------------------
// IBackingStore models the slow, authoritative storage that QuantumCache
// caches in front of.
class IBackingStore {
public:
    virtual ~IBackingStore() = default;

    // Returns ErrorCode::NotFound if the key has never been stored (or was
    // removed), which is a normal, expected outcome — not a failure of the
    // store itself. Any other failure (IoError, CorruptData) is a genuine
    // problem with the backing store.
    [[nodiscard]] virtual Common::Result<std::vector<std::uint8_t>> Get(
        const std::string& key) = 0;

    // Durably persists `value` under `key`, appending to the underlying
    // log and requiring a caller-visible success only once the write is
    // confirmed on stable storage.
    [[nodiscard]] virtual Common::Result<void> Put(
        const std::string& key, const std::vector<std::uint8_t>& value) = 0;

    // Durably persists a batch of records in a single physical append and flush.
    // The entire batch is committed atomically with a commit boundary and CRC.
    [[nodiscard]] virtual Common::Result<void> PutBatch(
        const std::vector<BackingStoreRecord>& records) {
        for (const auto& rec : records) {
            if (rec.tombstone) {
                auto res = Remove(rec.key);
                if (!res) return res;
            } else {
                auto res = Put(rec.key, rec.value);
                if (!res) return res;
            }
        }
        return Common::Result<void>::Success();
    }

    [[nodiscard]] virtual Common::Result<void> Remove(const std::string& key) = 0;

    [[nodiscard]] virtual bool Contains(const std::string& key) = 0;

    [[nodiscard]] virtual std::size_t EntryCount() const noexcept = 0;

    [[nodiscard]] virtual std::uint64_t GetVersion(const std::string& key) {
        (void)key;
        return 0;
    }
};

// Opens (or creates) a file-backed key/value store at `dataFilePath`.
[[nodiscard]] Common::Result<std::unique_ptr<IBackingStore>> OpenFileBackingStore(
    const std::wstring& dataFilePath);

} // namespace QuantumCache::Storage
