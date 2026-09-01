// Redesigned CacheEngine using Transactional Batch Storage Protocol.
//
// Key principles:
// 1. Backing Store Batch Durability:
//    - FlushAll() gathers ALL dirty entries across all shards into a single vector of Storage::BackingStoreRecord.
//    - Passes the vector to backingStore_->PutBatch(records), which appends them in one physical frame + durable flush.
// 2. Journal Durability:
//    - The WriteAheadJournal records Upsert and Invalidate entries.
//    - Per-key FlushIntent/FlushComplete records are removed.
//    - Upon successful PutBatch, journal_->Truncate() is called (if no remaining dirty entries in memory), safely reclaiming log space.
// 3. Versioning & Recovery:
//    - Strict per-key version ordering is maintained across RAM, journal, and backing store.
//    - ReplayFromJournal reconstructs dirty cache entries from journal records whose version is strictly greater than the version present in the backing store index.
//    - Idempotent recovery.

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

constexpr std::size_t kEntryOverheadBytes = 64;
constexpr std::size_t kMaxReasonableValueBytes = 64ull * 1024 * 1024;

enum class EngineLifecycle : int {
    NotReady = 0,
    Ready = 1,
    Stopping = 2,
    Stopped = 3,
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
    LruList lru; // front = MRU, back = LRU
    std::unordered_map<std::string, LruList::iterator> index;
    std::uint64_t currentBytes{0};
    std::uint64_t dirtyBytes{0};
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

        struct PendingEntry {
            std::vector<std::uint8_t> value;
            std::uint64_t version{0};
            bool isInvalidate{false};
        };
        std::unordered_map<std::string, PendingEntry> pending;

        auto replayResult = journal_->Replay(
            [&](const PowerResilience::JournalRecord& raw) -> Result<PowerResilience::ReplayAction> {
                auto decoded = JournalRecordCodec::Decode(raw.payload);
                if (!decoded) {
                    return Result<PowerResilience::ReplayAction>::Failure(decoded.Err());
                }

                const CacheJournalRecord& record = decoded.Value();
                switch (record.type) {
                    case CacheRecordType::Upsert:
                        pending[record.key] = PendingEntry{
                            record.value, record.entryVersion, /*isInvalidate=*/false};
                        break;

                    case CacheRecordType::Invalidate:
                        pending[record.key] = PendingEntry{
                            {}, record.entryVersion, /*isInvalidate=*/true};
                        break;

                    case CacheRecordType::FlushIntent:
                    case CacheRecordType::FlushComplete:
                        // Ignored in new durability model
                        break;
                }

                return Result<PowerResilience::ReplayAction>::Success(
                    PowerResilience::ReplayAction::Continue);
            });

        if (!replayResult) {
            lifecycle_.store(EngineLifecycle::RecoveryFailed);
            return replayResult;
        }

        std::uint64_t highestVersionSeen = 0;
        for (auto& [key, pendingEntry] : pending) {
            highestVersionSeen = std::max(highestVersionSeen, pendingEntry.version);
            std::uint64_t backingVersion = backingStore_->GetVersion(key);

            // Replay only if journal entry version > backing store version
            if (pendingEntry.version > backingVersion) {
                if (pendingEntry.isInvalidate) {
                    Storage::BackingStoreRecord removeRecord{key, {}, pendingEntry.version, /*tombstone=*/true};
                    auto removeRes = backingStore_->PutBatch({removeRecord});
                    if (!removeRes) {
                        lifecycle_.store(EngineLifecycle::RecoveryFailed);
                        return Result<void>::Failure(removeRes.Err());
                    }
                } else {
                    Shard& shard = ShardFor(key);
                    std::lock_guard<std::mutex> lock(shard.mutex);
                    InsertOrUpdateLocked(shard, key, pendingEntry.value, pendingEntry.version,
                                          EntryDirtyState::Dirty, /*enforceCapacity=*/false);
                }
            }
        }

        globalVersionCounter_.store(std::max(globalVersionCounter_.load(), highestVersionSeen));

        if (logger_ && !pending.empty()) {
            logger_->Log(Logging::LogLevel::Info, "CoreEngine",
                         "Replayed " + std::to_string(pending.size()) + " journal records during recovery.");
        }

        return Result<void>::Success();
    }

    Result<void> MarkRecoveryComplete() override {
        auto current = lifecycle_.load();
        if (current == EngineLifecycle::Ready) {
            return Result<void>::Success();
        }
        if (current == EngineLifecycle::RecoveryFailed) {
            return Result<void>::Failure(Error{
                ErrorCode::RecoveryFailed,
                "cannot mark recovery complete: a prior ReplayFromJournal() call failed", 0});
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

        StartBackgroundFlushThreadIfConfigured();
        return Result<void>::Success();
    }

    // -----------------------------------------------------------------
    // Read path.
    // -----------------------------------------------------------------

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
                shard.lru.splice(shard.lru.begin(), shard.lru, it->second);
                hitCount_.fetch_add(1, std::memory_order_relaxed);
                return Result<std::vector<std::uint8_t>>::Success(it->second->value);
            }
            fenceAtMiss = shard.mutationFence;
        }

        missCount_.fetch_add(1, std::memory_order_relaxed);

        auto backingResult = backingStore_->Get(key);
        if (!backingResult) {
            return Result<std::vector<std::uint8_t>>::Failure(backingResult.Err());
        }

        if (backingResult.Value().size() > kMaxReasonableValueBytes) {
            return Result<std::vector<std::uint8_t>>::Failure(Error{
                ErrorCode::CorruptData,
                "backing store returned an implausibly large value; refusing to cache or trust it", 0});
        }

        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            if (shard.mutationFence == fenceAtMiss) {
                InsertOrUpdateLocked(shard, key, backingResult.Value(), /*version=*/0,
                                      EntryDirtyState::Clean, /*enforceCapacity=*/true);
                return Result<std::vector<std::uint8_t>>::Success(backingResult.Value());
            }

            auto it = shard.index.find(key);
            if (it != shard.index.end()) {
                shard.lru.splice(shard.lru.begin(), shard.lru, it->second);
                hitCount_.fetch_add(1, std::memory_order_relaxed);
                return Result<std::vector<std::uint8_t>>::Success(it->second->value);
            }
        }

        return Result<std::vector<std::uint8_t>>::Success(backingResult.Value());
    }

    bool Contains(const std::string& key) override {
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
        auto readyCheck = CheckReadyForRead();
        if (!readyCheck) return Result<void>::Failure(readyCheck.Err());
        return FlushKey(key, /*expectedVersion=*/0, /*checkVersion=*/false);
    }

    Result<void> FlushAll() override {
        auto readyCheck = CheckReadyForRead();
        if (!readyCheck) return Result<void>::Failure(readyCheck.Err());

        struct FlushItem {
            std::string key;
            std::vector<std::uint8_t> value;
            std::uint64_t version;
        };
        std::vector<FlushItem> itemsToFlush;

        // Step 1: Gather dirty items across all shards and mark them FlushInProgress
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            for (auto& entry : shard.lru) {
                if (entry.dirtyState == EntryDirtyState::Dirty) {
                    entry.dirtyState = EntryDirtyState::FlushInProgress;
                    itemsToFlush.push_back(FlushItem{entry.key, entry.value, entry.version});
                }
            }
        }

        if (itemsToFlush.empty()) {
            TryCompactJournalIfFullyClean();
            return Result<void>::Success();
        }

        // Step 2: Build backing store records for batch write
        std::vector<Storage::BackingStoreRecord> batchRecords;
        batchRecords.reserve(itemsToFlush.size());
        for (const auto& item : itemsToFlush) {
            batchRecords.push_back(Storage::BackingStoreRecord{
                item.key, item.value, item.version, /*tombstone=*/false});
        }

        // Step 3: Issue single durable batch write to backing store
        auto batchResult = backingStore_->PutBatch(batchRecords);
        if (!batchResult) {
            // Revert FlushInProgress items back to Dirty
            for (const auto& item : itemsToFlush) {
                Shard& shard = ShardFor(item.key);
                std::lock_guard<std::mutex> lock(shard.mutex);
                auto it = shard.index.find(item.key);
                if (it != shard.index.end() && it->second->version == item.version &&
                    it->second->dirtyState == EntryDirtyState::FlushInProgress) {
                    it->second->dirtyState = EntryDirtyState::Dirty;
                }
            }
            flushFailureCount_.fetch_add(itemsToFlush.size(), std::memory_order_relaxed);
            if (logger_) {
                logger_->Log(Logging::LogLevel::Error, "CoreEngine",
                             "FlushAll: PutBatch failed: " + batchResult.Err().message);
            }
            return Result<void>::Failure(batchResult.Err());
        }

        // Step 4: Mark items as Clean if version hasn't changed
        for (const auto& item : itemsToFlush) {
            Shard& shard = ShardFor(item.key);
            std::lock_guard<std::mutex> lock(shard.mutex);
            auto it = shard.index.find(item.key);
            if (it != shard.index.end() && it->second->version == item.version &&
                it->second->dirtyState == EntryDirtyState::FlushInProgress) {
                std::size_t sizeBytes = it->second->SizeBytes();
                it->second->dirtyState = EntryDirtyState::Clean;
                shard.dirtyBytes -= sizeBytes;
                --shard.dirtyCount;
            }
        }

        flushSuccessCount_.fetch_add(itemsToFlush.size(), std::memory_order_relaxed);

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
                return Result<void>::Success();
            }
            return Result<void>::Failure(Error{
                ErrorCode::InvalidArgument, "cannot shut down an engine that was never marked ready", 0});
        }

        if (logger_) {
            logger_->Log(Logging::LogLevel::Info, "CoreEngine",
                         "Cache engine shutting down: flushing dirty entries.");
        }

        DrainInFlightWrites();
        StopBackgroundFlushThread();

        auto flushResult = FlushAll();
        if (!flushResult && logger_) {
            logger_->Log(Logging::LogLevel::Warning, "CoreEngine",
                         "Shutdown: FlushAll did not fully succeed (" + flushResult.Err().message + ").");
        }

        lifecycle_.store(EngineLifecycle::Stopped);

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
                             "Shutdown: journal truncation failed (" + truncateResult.Err().message + ").");
            }
        }

        return Result<void>::Success();
    }

private:
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

    void InsertOrUpdateLocked(Shard& shard, const std::string& key,
                               const std::vector<std::uint8_t>& value, std::uint64_t version,
                               EntryDirtyState state, bool enforceCapacity) {
        auto it = shard.index.find(key);
        bool isUpdate = it != shard.index.end();

        if (isUpdate) {
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

        ++shard.mutationFence;

        if (enforceCapacity) {
            EvictCleanEntriesLocked(shard);
        }
    }

    void EvictCleanEntriesLocked(Shard& shard) {
        while ((shard.currentBytes > perShardCapacityBytes_ ||
                shard.index.size() > perShardMaxEntries_) &&
               shard.index.size() > shard.dirtyCount) {
            auto it = shard.lru.rbegin();
            while (it != shard.lru.rend() && it->dirtyState != EntryDirtyState::Clean) {
                ++it;
            }
            if (it == shard.lru.rend()) {
                break;
            }
            auto forwardIt = std::next(it).base();
            shard.currentBytes -= forwardIt->SizeBytes();
            shard.index.erase(forwardIt->key);
            shard.lru.erase(forwardIt);
            evictionCount_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    static constexpr int kMaxFlushAttemptsPerCall = 64;

    void RelieveCapacityPressureIfNeeded(Shard& shard) {
        for (int attempt = 0; attempt < kMaxFlushAttemptsPerCall; ++attempt) {
            std::string keyToFlush;
            std::size_t dirtyCountBefore = 0;
            {
                std::lock_guard<std::mutex> lock(shard.mutex);
                bool overBudget = shard.currentBytes > perShardCapacityBytes_ ||
                                   shard.index.size() > perShardMaxEntries_;
                if (!overBudget) return;

                dirtyCountBefore = shard.dirtyCount;

                for (auto it = shard.lru.rbegin(); it != shard.lru.rend(); ++it) {
                    if (it->dirtyState == EntryDirtyState::Dirty) {
                        keyToFlush = it->key;
                        break;
                    }
                }

                if (keyToFlush.empty()) {
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
                return;
            }

            if (dirtyCountAfter >= dirtyCountBefore) {
                return;
            }
        }
    }

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
                return Result<void>::Success();
            }
            if (it->second->dirtyState == EntryDirtyState::Clean) {
                return Result<void>::Success();
            }
            if (it->second->dirtyState == EntryDirtyState::FlushInProgress) {
                return Result<void>::Success();
            }

            value = it->second->value;
            version = it->second->version;
            it->second->dirtyState = EntryDirtyState::FlushInProgress;
        }

        Storage::BackingStoreRecord record{key, value, version, /*tombstone=*/false};
        auto putResult = backingStore_->PutBatch({record});
        if (!putResult) {
            RevertFlushInProgressLocked(shard, key, version);
            flushFailureCount_.fetch_add(1, std::memory_order_relaxed);
            return putResult;
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
        }

        flushSuccessCount_.fetch_add(1, std::memory_order_relaxed);
        return Result<void>::Success();
    }

    void RevertFlushInProgressLocked(Shard& shard, const std::string& key, std::uint64_t version) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto it = shard.index.find(key);
        if (it != shard.index.end() && it->second->version == version &&
            it->second->dirtyState == EntryDirtyState::FlushInProgress) {
            it->second->dirtyState = EntryDirtyState::Dirty;
        }
    }

    Result<void> InvalidateImpl(const std::string& key, bool force) {
        auto readyCheck = CheckReadyForWrite();
        if (!readyCheck) return readyCheck;

        if (!TryAdmitWrite()) {
            return Result<void>::Failure(Error{
                ErrorCode::ServiceStopping,
                "cache engine is shutting down and no longer accepts new writes", 0});
        }
        WriteAdmissionGuard admissionGuard(*this);

        Shard& shard = ShardFor(key);
        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            auto it = shard.index.find(key);
            if (it != shard.index.end()) {
                if (!force && it->second->dirtyState != EntryDirtyState::Clean) {
                    return Result<void>::Failure(Error{
                        ErrorCode::UnflushedDirtyData,
                        "refusing to invalidate '" + key +
                            "': it has unflushed dirty data (use ForceInvalidate to override)", 0});
                }
            }
        }

        CacheJournalRecord record;
        record.type = CacheRecordType::Invalidate;
        record.key = key;
        std::uint64_t invVersion = 0;
        auto journalResult = AppendJournalRecordWithNewVersion(record, invVersion);
        if (!journalResult) return journalResult;

        Storage::BackingStoreRecord removeRecord{key, {}, invVersion, /*tombstone=*/true};
        auto backingResult = backingStore_->PutBatch({removeRecord});
        if (!backingResult) {
            return backingResult;
        }

        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            auto it = shard.index.find(key);
            if (it != shard.index.end() && it->second->version <= invVersion) {
                std::size_t sizeBytes = it->second->SizeBytes();
                if (it->second->dirtyState != EntryDirtyState::Clean) {
                    shard.dirtyBytes -= sizeBytes;
                    --shard.dirtyCount;
                }
                shard.currentBytes -= sizeBytes;
                shard.lru.erase(it->second);
                shard.index.erase(it);
            }
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

    std::mutex admissionMutex_;
    std::condition_variable admissionDrainedCv_;
    std::uint64_t inFlightWriteCount_{0};
    bool compactionPaused_{false};

    [[nodiscard]] bool TryAdmitWrite() {
        std::unique_lock<std::mutex> lock(admissionMutex_);
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

    void DrainInFlightWrites() {
        std::unique_lock<std::mutex> lock(admissionMutex_);
        admissionDrainedCv_.wait(lock, [this]() { return inFlightWriteCount_ == 0; });
    }

    void TryCompactJournalIfFullyClean() {
        {
            std::lock_guard<std::mutex> journalLock(journalMutex_);
            if (journal_->RecordCount() == 0) {
                return;
            }
        }

        {
            std::lock_guard<std::mutex> lock(admissionMutex_);
            compactionPaused_ = true;
        }

        DrainInFlightWrites();

        std::uint64_t remainingDirty = 0;
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            remainingDirty += shard.dirtyCount;
        }

        if (remainingDirty == 0) {
            std::lock_guard<std::mutex> journalLock(journalMutex_);
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
                                 truncateResult.Err().message + ").");
            } else if (truncateResult && logger_) {
                logger_->Log(Logging::LogLevel::Info, "CoreEngine",
                             "Opportunistic journal compaction: journal truncated.");
            }
        }

        {
            std::lock_guard<std::mutex> lock(admissionMutex_);
            compactionPaused_ = false;
        }
        admissionDrainedCv_.notify_all();
    }

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

    std::thread backgroundFlushThread_;
    std::mutex backgroundFlushMutex_;
    std::condition_variable backgroundFlushCv_;
    bool backgroundFlushStopRequested_{false};

    void StartBackgroundFlushThreadIfConfigured() {
        if (options_.flushPolicy != FlushPolicyKind::PeriodicBackground) return;
        if (backgroundFlushThread_.joinable()) return;

        std::uint32_t intervalSeconds = std::max<std::uint32_t>(1, options_.flushIntervalSeconds);
        backgroundFlushStopRequested_ = false;

        backgroundFlushThread_ = std::thread([this, intervalSeconds]() {
            std::unique_lock<std::mutex> lock(backgroundFlushMutex_);
            while (!backgroundFlushStopRequested_) {
                backgroundFlushCv_.wait_for(
                    lock, std::chrono::seconds(intervalSeconds),
                    [this]() { return backgroundFlushStopRequested_; });
                if (backgroundFlushStopRequested_) break;

                lock.unlock();
                auto lifecycleNow = lifecycle_.load();
                if (lifecycleNow == EngineLifecycle::Ready) {
                    auto flushResult = FlushAll();
                    if (!flushResult && logger_) {
                        logger_->Log(Logging::LogLevel::Warning, "CoreEngine",
                                     "Periodic background flush did not fully succeed: " +
                                         flushResult.Err().message);
                    }
                }
                lock.lock();
            }
        });

        if (logger_) {
            logger_->Log(Logging::LogLevel::Info, "CoreEngine",
                         "Periodic background flush thread started.");
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
