// Clean, redesign of FileBackingStore using a Transactional Batch Durability Protocol.
//
// On-disk record framing (batch-oriented, durable commit boundary):
//
//  Batch Header:
//    uint32_t batchMagic     = 0x51434248 ('Q','C','B','H')
//    uint64_t batchId
//    uint32_t recordCount
//
//  Per Record (1..recordCount):
//    uint64_t version
//    uint8_t  tombstone       (0 = value present, 1 = key removed)
//    uint32_t keyLength
//    uint8_t  key[keyLength]
//    uint32_t valueLength     (0 when tombstone == 1)
//    uint8_t  value[valueLength]
//
//  Batch Commit Marker:
//    uint32_t commitMagic    = 0x51434243 ('Q','C','B','C')
//    uint64_t batchId        (must match header batchId)
//    uint32_t crc32          (computed over batch header + all records + commit magic + batchId)
//
// Startup behavior:
//   OpenFileBackingStore scans the file sequentially batch by batch.
//   A batch is valid ONLY if its header, all records, commit marker, and CRC match.
//   If an uncommitted or torn batch is encountered (CRC mismatch, missing commit marker, short read),
//   the scan stops immediately and the file is SetLength()-truncated to the end of the last
//   valid committed batch and durably flushed.
//   In-memory index is updated only for committed batches using strict version ordering (newer or equal version wins).

#include "QuantumCache/Storage/IBackingStore.h"
#include "QuantumCache/Storage/IFile.h"
#include "QuantumCache/Common/Crc32.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace QuantumCache::Storage {
namespace {

using Common::Crc32;
using Common::Error;
using Common::ErrorCode;
using Common::Result;

constexpr std::uint32_t kBatchHeaderMagic = 0x51434248u; // "QCBH"
constexpr std::uint32_t kBatchCommitMagic = 0x51434243u; // "QCBC"
constexpr std::uint32_t kMaxReasonableLength = 256u * 1024u * 1024u;

struct IndexEntry {
    std::uint64_t recordOffset{0};
    bool tombstone{false};
    std::uint64_t version{0};
};

template <typename T>
void AppendRaw(std::vector<std::uint8_t>& buffer, const T& value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    buffer.insert(buffer.end(), bytes, bytes + sizeof(T));
}

class FileBackingStore final : public IBackingStore {
public:
    explicit FileBackingStore(std::unique_ptr<IFile> file) : file_(std::move(file)) {}

    Result<void> Initialize() {
        std::uint64_t offset = 0;
        std::uint64_t validEnd = 0;
        std::uint64_t maxBatchId = 0;
        std::uint64_t maxVersion = 0;

        auto seekResult = file_->Seek(0, false);
        if (!seekResult) return Result<void>::Failure(seekResult.Err());

        for (;;) {
            std::uint64_t batchStartOffset = offset;
            std::uint32_t headerMagic = 0;
            if (!ReadExact(&headerMagic, sizeof(headerMagic))) break;
            if (headerMagic != kBatchHeaderMagic) break;

            std::uint64_t batchId = 0;
            std::uint32_t recordCount = 0;
            if (!ReadExact(&batchId, sizeof(batchId))) break;
            if (!ReadExact(&recordCount, sizeof(recordCount))) break;

            std::vector<std::uint8_t> batchDataForCrc;
            AppendRaw(batchDataForCrc, headerMagic);
            AppendRaw(batchDataForCrc, batchId);
            AppendRaw(batchDataForCrc, recordCount);

            struct ScannedRecord {
                std::string key;
                std::uint64_t recordOffset;
                bool tombstone;
                std::uint64_t version;
            };
            std::vector<ScannedRecord> scannedRecords;
            scannedRecords.reserve(recordCount);

            bool batchReadOk = true;
            std::uint64_t currentRecordOffset = batchStartOffset + sizeof(headerMagic) + sizeof(batchId) + sizeof(recordCount);

            for (std::uint32_t r = 0; r < recordCount; ++r) {
                std::uint64_t version = 0;
                std::uint8_t tombstone = 0;
                std::uint32_t keyLength = 0;

                if (!ReadExact(&version, sizeof(version)) ||
                    !ReadExact(&tombstone, sizeof(tombstone)) ||
                    !ReadExact(&keyLength, sizeof(keyLength))) {
                    batchReadOk = false;
                    break;
                }

                if (keyLength > kMaxReasonableLength) {
                    batchReadOk = false;
                    break;
                }

                std::vector<std::uint8_t> keyBytes(keyLength);
                if (keyLength > 0 && !ReadExact(keyBytes.data(), keyLength)) {
                    batchReadOk = false;
                    break;
                }

                std::uint32_t valueLength = 0;
                if (!ReadExact(&valueLength, sizeof(valueLength))) {
                    batchReadOk = false;
                    break;
                }
                if (valueLength > kMaxReasonableLength) {
                    batchReadOk = false;
                    break;
                }

                std::vector<std::uint8_t> valueBytes(valueLength);
                if (valueLength > 0 && !ReadExact(valueBytes.data(), valueLength)) {
                    batchReadOk = false;
                    break;
                }

                AppendRaw(batchDataForCrc, version);
                AppendRaw(batchDataForCrc, tombstone);
                AppendRaw(batchDataForCrc, keyLength);
                batchDataForCrc.insert(batchDataForCrc.end(), keyBytes.begin(), keyBytes.end());
                AppendRaw(batchDataForCrc, valueLength);
                batchDataForCrc.insert(batchDataForCrc.end(), valueBytes.begin(), valueBytes.end());

                std::string key(keyBytes.begin(), keyBytes.end());
                scannedRecords.push_back(ScannedRecord{key, currentRecordOffset, tombstone != 0, version});

                currentRecordOffset += sizeof(version) + sizeof(tombstone) + sizeof(keyLength) +
                                       keyLength + sizeof(valueLength) + valueLength;
            }

            if (!batchReadOk) break;

            std::uint32_t commitMagic = 0;
            std::uint64_t commitBatchId = 0;
            std::uint32_t storedCrc = 0;

            if (!ReadExact(&commitMagic, sizeof(commitMagic)) ||
                !ReadExact(&commitBatchId, sizeof(commitBatchId)) ||
                commitMagic != kBatchCommitMagic ||
                commitBatchId != batchId) {
                break;
            }

            AppendRaw(batchDataForCrc, commitMagic);
            AppendRaw(batchDataForCrc, commitBatchId);

            if (!ReadExact(&storedCrc, sizeof(storedCrc))) break;

            std::uint32_t computedCrc = Crc32::Compute(batchDataForCrc.data(), batchDataForCrc.size());
            if (computedCrc != storedCrc) break; // Torn batch or corruption; stop here.

            // Batch is completely committed! Update in-memory index with version ordering.
            for (const auto& rec : scannedRecords) {
                auto it = index_.find(rec.key);
                if (it == index_.end() || rec.version >= it->second.version) {
                    index_[rec.key] = IndexEntry{rec.recordOffset, rec.tombstone, rec.version};
                }
                maxVersion = std::max(maxVersion, rec.version);
            }

            maxBatchId = std::max(maxBatchId, batchId);

            offset = currentRecordOffset + sizeof(commitMagic) + sizeof(commitBatchId) + sizeof(storedCrc);
            validEnd = offset;
        }

        nextBatchId_ = maxBatchId + 1;
        nextVersion_ = maxVersion + 1;
        endOffset_ = validEnd;

        // Truncate any incomplete batch at EOF.
        auto sizeResult = file_->Size();
        if (sizeResult && sizeResult.Value() != validEnd) {
            auto setLenResult = file_->SetLength(validEnd);
            if (!setLenResult) return setLenResult;
            auto flushResult = file_->FlushDurable();
            if (!flushResult) return flushResult;
        }

        return Result<void>::Success();
    }

    Result<std::vector<std::uint8_t>> Get(const std::string& key) override {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = index_.find(key);
        if (it == index_.end() || it->second.tombstone) {
            return Result<std::vector<std::uint8_t>>::Failure(
                Error{ErrorCode::NotFound, "key not present in backing store: " + key, 0});
        }

        auto seekResult = file_->Seek(static_cast<std::int64_t>(it->second.recordOffset), false);
        if (!seekResult) return Result<std::vector<std::uint8_t>>::Failure(seekResult.Err());

        std::uint64_t version = 0;
        std::uint8_t tombstone = 0;
        std::uint32_t keyLength = 0;
        if (!ReadExact(&version, sizeof(version)) ||
            !ReadExact(&tombstone, sizeof(tombstone)) ||
            !ReadExact(&keyLength, sizeof(keyLength))) {
            return Result<std::vector<std::uint8_t>>::Failure(
                Error{ErrorCode::CorruptData, "backing store record header unreadable at indexed offset", 0});
        }
        std::vector<std::uint8_t> keyBytes(keyLength);
        if (keyLength > 0 && !ReadExact(keyBytes.data(), keyLength)) {
            return Result<std::vector<std::uint8_t>>::Failure(
                Error{ErrorCode::CorruptData, "backing store record key unreadable at indexed offset", 0});
        }
        std::uint32_t valueLength = 0;
        if (!ReadExact(&valueLength, sizeof(valueLength))) {
            return Result<std::vector<std::uint8_t>>::Failure(
                Error{ErrorCode::CorruptData, "backing store record value length unreadable", 0});
        }
        std::vector<std::uint8_t> valueBytes(valueLength);
        if (valueLength > 0 && !ReadExact(valueBytes.data(), valueLength)) {
            return Result<std::vector<std::uint8_t>>::Failure(
                Error{ErrorCode::CorruptData, "backing store record value unreadable", 0});
        }

        return Result<std::vector<std::uint8_t>>::Success(std::move(valueBytes));
    }

    Result<void> Put(const std::string& key, const std::vector<std::uint8_t>& value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::uint64_t version = nextVersion_++;
        BackingStoreRecord record{key, value, version, /*tombstone=*/false};
        return AppendBatchLocked({record});
    }

    Result<void> PutBatch(const std::vector<BackingStoreRecord>& records) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (records.empty()) {
            return Result<void>::Success();
        }
        return AppendBatchLocked(records);
    }

    Result<void> Remove(const std::string& key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = index_.find(key);
        if (it == index_.end() || it->second.tombstone) {
            return Result<void>::Success(); // idempotent
        }
        std::uint64_t version = nextVersion_++;
        BackingStoreRecord record{key, {}, version, /*tombstone=*/true};
        return AppendBatchLocked({record});
    }

    bool Contains(const std::string& key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = index_.find(key);
        return it != index_.end() && !it->second.tombstone;
    }

    std::size_t EntryCount() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t count = 0;
        for (auto& [key, entry] : index_) {
            if (!entry.tombstone) ++count;
        }
        return count;
    }

    std::uint64_t GetVersion(const std::string& key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = index_.find(key);
        if (it == index_.end()) return 0;
        return it->second.version;
    }

private:
    Result<void> AppendBatchLocked(const std::vector<BackingStoreRecord>& records) {
        std::uint64_t batchId = nextBatchId_++;

        std::vector<std::uint8_t> frame;
        AppendRaw(frame, kBatchHeaderMagic);
        AppendRaw(frame, batchId);
        AppendRaw(frame, static_cast<std::uint32_t>(records.size()));

        struct PendingIndex {
            std::string key;
            std::uint64_t recordOffset;
            bool tombstone;
            std::uint64_t version;
        };
        std::vector<PendingIndex> pendingIndex;
        pendingIndex.reserve(records.size());

        for (const auto& rec : records) {
            std::uint64_t recordOffset = endOffset_ + frame.size();
            std::uint8_t tombstoneByte = rec.tombstone ? 1 : 0;
            std::uint32_t keyLen = static_cast<std::uint32_t>(rec.key.size());
            std::uint32_t valLen = static_cast<std::uint32_t>(rec.tombstone ? 0 : rec.value.size());

            AppendRaw(frame, rec.version);
            AppendRaw(frame, tombstoneByte);
            AppendRaw(frame, keyLen);
            frame.insert(frame.end(), rec.key.begin(), rec.key.end());
            AppendRaw(frame, valLen);
            if (!rec.tombstone && valLen > 0) {
                frame.insert(frame.end(), rec.value.begin(), rec.value.end());
            }

            pendingIndex.push_back(PendingIndex{rec.key, recordOffset, rec.tombstone, rec.version});
            nextVersion_ = std::max(nextVersion_, rec.version + 1);
        }

        AppendRaw(frame, kBatchCommitMagic);
        AppendRaw(frame, batchId);

        std::uint32_t crc = Crc32::Compute(frame.data(), frame.size());
        AppendRaw(frame, crc);

        auto seekResult = file_->Seek(static_cast<std::int64_t>(endOffset_), false);
        if (!seekResult) return Result<void>::Failure(seekResult.Err());

        auto writeResult = file_->Write(frame.data(), frame.size());
        if (!writeResult || writeResult.Value() != frame.size()) {
            return Result<void>::Failure(
                Error{ErrorCode::IoError, "backing store batch append: short write", 0});
        }

        // Single durable flush for the entire batch.
        auto flushResult = file_->FlushDurable();
        if (!flushResult) return flushResult;

        // Apply to in-memory index only after durability is confirmed!
        for (const auto& p : pendingIndex) {
            auto it = index_.find(p.key);
            if (it == index_.end() || p.version >= it->second.version) {
                index_[p.key] = IndexEntry{p.recordOffset, p.tombstone, p.version};
            }
        }

        endOffset_ += frame.size();
        return Result<void>::Success();
    }

    bool ReadExact(void* buffer, std::size_t bytes) {
        auto readResult = file_->Read(buffer, bytes);
        return readResult && readResult.Value() == bytes;
    }

    std::unique_ptr<IFile> file_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, IndexEntry> index_;
    std::uint64_t endOffset_{0};
    std::uint64_t nextBatchId_{1};
    std::uint64_t nextVersion_{1};
};

} // namespace

Result<std::unique_ptr<IBackingStore>> OpenFileBackingStore(const std::wstring& dataFilePath) {
    auto fileResult = OpenFile(dataFilePath, OpenMode::OpenOrCreate);
    if (!fileResult) {
        return Result<std::unique_ptr<IBackingStore>>::Failure(fileResult.Err());
    }

    auto store = std::make_unique<FileBackingStore>(std::move(fileResult.Value()));
    auto initResult = store->Initialize();
    if (!initResult) {
        return Result<std::unique_ptr<IBackingStore>>::Failure(initResult.Err());
    }

    return Result<std::unique_ptr<IBackingStore>>::Success(std::move(store));
}

} // namespace QuantumCache::Storage
