// Real, working file-backed key/value store used as the Stage 2 backing
// store CacheEngine caches in front of. Built entirely on IFile (works
// against both Win32File and PortableFile — no platform-specific code
// here), so it inherits IFile's existing crash-consistency behavior
// rather than inventing a second one.
//
// On-disk record framing (append-only log, same shape of reasoning as
// PowerResilience's write-ahead journal, deliberately reusing the pattern
// rather than a third bespoke format):
//   uint32_t magic        = 0x51434253 ('Q','C','B','S')
//   uint64_t sequence
//   uint8_t  tombstone     (0 = value present, 1 = key removed)
//   uint32_t keyLength
//   uint8_t  key[keyLength]
//   uint32_t valueLength   (0 when tombstone == 1)
//   uint8_t  value[valueLength]
//   uint32_t crc32         (over every byte above, this record only)
//
// Startup behavior: OpenFileBackingStore scans the whole file sequentially
// (exactly like IWriteAheadJournal::Replay), building an in-memory index
// of "latest record offset per key". If a torn/corrupt record is found
// (CRC mismatch, truncated read — the expected shape of a power cut
// mid-append), the scan stops there and the file is immediately
// SetLength()-truncated to the end of the last valid record and durably
// flushed, so the torn garbage can never be misread later and new writes
// land at a clean append point.
#include "QuantumCache/Storage/IBackingStore.h"
#include "QuantumCache/Storage/IFile.h"
#include "QuantumCache/Common/Crc32.h"
#include <cstring>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace QuantumCache::Storage {
namespace {

using Common::Crc32;
using Common::Error;
using Common::ErrorCode;
using Common::Result;

constexpr std::uint32_t kMagic = 0x51434253u; // "QCBS"

// AUDITED BUG (fixed): Initialize()'s startup scan used to allocate
// std::vector<std::uint8_t> keyBytes(keyLength) / valueBytes(valueLength)
// directly from uint32_t length fields read straight off disk, with NO
// sanity bound — the same class of issue fixed in
// WriteAheadJournal::Replay() (see that file's kMaxReasonablePayloadLength
// comment for the full rationale). A single corrupted length byte in the
// backing-store file (again, exactly the kind of damage a torn write or
// bad sector causes) could request a multi-gigabyte allocation during
// startup, causing an uncaught std::bad_alloc/OOM crash instead of the
// intended "stop scan, treat as torn tail" behavior every other
// corruption case in this same loop already gets.
constexpr std::uint32_t kMaxReasonableLength = 256u * 1024u * 1024u;

struct IndexEntry {
    std::uint64_t recordOffset{0};
    bool tombstone{false};
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
        std::uint64_t maxSequence = 0;

        auto seekResult = file_->Seek(0, false);
        if (!seekResult) return Result<void>::Failure(seekResult.Err());

        for (;;) {
            std::uint32_t magic = 0;
            if (!ReadExact(&magic, sizeof(magic))) break;
            if (magic != kMagic) break;

            std::uint64_t sequence = 0;
            std::uint8_t tombstone = 0;
            std::uint32_t keyLength = 0;
            if (!ReadExact(&sequence, sizeof(sequence))) break;
            if (!ReadExact(&tombstone, sizeof(tombstone))) break;
            if (!ReadExact(&keyLength, sizeof(keyLength))) break;
            // AUDITED BUG (fixed): bound-check BEFORE allocating — see
            // kMaxReasonableLength's comment above.
            if (keyLength > kMaxReasonableLength) break;

            std::vector<std::uint8_t> keyBytes(keyLength);
            if (keyLength > 0 && !ReadExact(keyBytes.data(), keyLength)) break;

            std::uint32_t valueLength = 0;
            if (!ReadExact(&valueLength, sizeof(valueLength))) break;
            if (valueLength > kMaxReasonableLength) break;

            std::vector<std::uint8_t> valueBytes(valueLength);
            if (valueLength > 0 && !ReadExact(valueBytes.data(), valueLength)) break;

            std::uint32_t storedCrc = 0;
            if (!ReadExact(&storedCrc, sizeof(storedCrc))) break;

            std::vector<std::uint8_t> forCrc;
            AppendRaw(forCrc, magic);
            AppendRaw(forCrc, sequence);
            AppendRaw(forCrc, tombstone);
            AppendRaw(forCrc, keyLength);
            forCrc.insert(forCrc.end(), keyBytes.begin(), keyBytes.end());
            AppendRaw(forCrc, valueLength);
            forCrc.insert(forCrc.end(), valueBytes.begin(), valueBytes.end());
            std::uint32_t computedCrc = Crc32::Compute(forCrc.data(), forCrc.size());

            if (computedCrc != storedCrc) break; // torn tail; stop here.

            std::string key(keyBytes.begin(), keyBytes.end());
            index_[key] = IndexEntry{offset, tombstone != 0};
            maxSequence = std::max(maxSequence, sequence);

            // Recompute offset as "end of this record" for the next
            // iteration and as the running valid-data watermark.
            offset = offset
                     + sizeof(magic) + sizeof(sequence) + sizeof(tombstone) + sizeof(keyLength)
                     + keyLength + sizeof(valueLength) + valueLength + sizeof(storedCrc);
            validEnd = offset;
        }

        nextSequence_ = maxSequence + 1;
        endOffset_ = validEnd;

        // Drop any torn/garbage tail bytes beyond the last valid record so
        // future appends start from a clean, known-good point and nothing
        // could ever later misinterpret the discarded bytes.
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

        std::uint32_t magic = 0, keyLength = 0, valueLength = 0;
        std::uint64_t sequence = 0;
        std::uint8_t tombstone = 0;
        if (!ReadExact(&magic, sizeof(magic)) || magic != kMagic ||
            !ReadExact(&sequence, sizeof(sequence)) ||
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
        return AppendRecord(key, value, /*tombstone=*/false);
    }

    Result<void> Remove(const std::string& key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = index_.find(key);
        if (it == index_.end() || it->second.tombstone) {
            return Result<void>::Success(); // idempotent: nothing to remove.
        }
        return AppendRecord(key, {}, /*tombstone=*/true);
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

private:
    Result<void> AppendRecord(const std::string& key, const std::vector<std::uint8_t>& value,
                               bool tombstone) {
        std::uint64_t sequence = nextSequence_;
        std::uint8_t tombstoneByte = tombstone ? 1 : 0;

        std::vector<std::uint8_t> frame;
        frame.reserve(sizeof(kMagic) + sizeof(sequence) + sizeof(tombstoneByte) +
                      sizeof(std::uint32_t) + key.size() +
                      sizeof(std::uint32_t) + (tombstone ? 0 : value.size()) +
                      sizeof(std::uint32_t));
        AppendRaw(frame, kMagic);
        AppendRaw(frame, sequence);
        AppendRaw(frame, tombstoneByte);
        AppendRaw(frame, static_cast<std::uint32_t>(key.size()));
        frame.insert(frame.end(), key.begin(), key.end());
        AppendRaw(frame, static_cast<std::uint32_t>(tombstone ? 0 : value.size()));
        if (!tombstone) {
            frame.insert(frame.end(), value.begin(), value.end());
        }
        std::uint32_t crc = Crc32::Compute(frame.data(), frame.size());
        AppendRaw(frame, crc);

        auto seekResult = file_->Seek(static_cast<std::int64_t>(endOffset_), false);
        if (!seekResult) return Result<void>::Failure(seekResult.Err());

        auto writeResult = file_->Write(frame.data(), frame.size());
        if (!writeResult || writeResult.Value() != frame.size()) {
            return Result<void>::Failure(
                Error{ErrorCode::IoError, "backing store append: short write", 0});
        }

        // Never acknowledge this Put/Remove as durable before the actual
        // flush-to-stable-media boundary is reached (same rule the
        // write-ahead journal follows).
        auto flushResult = file_->FlushDurable();
        if (!flushResult) return flushResult;

        index_[key] = IndexEntry{endOffset_, tombstone};
        endOffset_ += frame.size();
        ++nextSequence_;

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
    std::uint64_t nextSequence_{0};
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
