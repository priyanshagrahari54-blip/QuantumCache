// Stage 2A verification: the real in-memory cache core, in isolation.
//
// This file deliberately exercises ONLY the plain in-memory cache-core
// surface Stage 2A was scoped to: real cache entries + memory accounting,
// Get/insert/update/invalidate, configurable capacity, real LRU eviction,
// accurate hit/miss counters, thread-safe concurrent access, and explicit
// error handling. It does not exercise (and does not depend on) the
// deferred-write / dirty-state / flush machinery that also happens to
// exist in this codebase — every test here uses
// WritePolicyKind::WriteThrough, so every successful Put() lands the
// entry as Clean immediately and the dirty/Flush machinery never enters
// the picture. Existing tests in CacheEngineTests.cpp already cover the
// deferred-write/journal-replay/recovery surface; this file exists so
// Stage 2A's specific requirements have their own unambiguous,
// standalone verification independent of that broader functionality.
//
// Nothing here is a mock or simulation: every test drives the real
// CacheEngine implementation (src/CoreEngine/src/CacheEngine.cpp) against
// a real Storage::IBackingStore (FileBackingStore, backed by a real
// temp-directory file) and a real PowerResilience::IWriteAheadJournal —
// the same production types used everywhere else in this project.
#include "QuantumCache/CoreEngine/ICacheEngine.h"
#include "QuantumCache/PowerResilience/IWriteAheadJournal.h"
#include "QuantumCache/Storage/IBackingStore.h"
#include "QuantumCache/Storage/IFile.h"
#include <gtest/gtest.h>
#include <atomic>
#include <filesystem>
#include <thread>

using namespace QuantumCache;
namespace fs = std::filesystem;

namespace {

std::vector<std::uint8_t> Bytes(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

std::string ToStr(const std::vector<std::uint8_t>& b) {
    return std::string(b.begin(), b.end());
}

class CacheEngineCoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        testDir_ = fs::temp_directory_path() /
                   ("qc_cache_core_test_" +
                    std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
                    std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(testDir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(testDir_, ec);
    }

    std::wstring PathFor(const std::string& name) const {
        std::wstring wide;
        std::string narrow = (testDir_ / name).string();
        for (char c : narrow) wide.push_back(static_cast<wchar_t>(c));
        return wide;
    }

    // Builds a ready-to-use cache core: real journal + real file-backed
    // backing store + real CacheEngine, forced to WriteThrough so every
    // successful Put() is immediately Clean — isolating pure cache-core
    // behavior (Stage 2A) from the deferred-write state model.
    struct Rig {
        std::shared_ptr<PowerResilience::IWriteAheadJournal> journal;
        std::shared_ptr<Storage::IBackingStore> backingStore;
        std::unique_ptr<CoreEngine::ICacheEngine> engine;
    };

    Rig BuildReadyRig(const std::string& stem, CoreEngine::CacheEngineOptions options = {}) {
        options.writePolicy = CoreEngine::WritePolicyKind::WriteThrough;

        Rig rig;
        auto journalFile = Storage::OpenFile(PathFor(stem + ".journal"), Storage::OpenMode::OpenOrCreate);
        EXPECT_TRUE(journalFile.IsOk());
        auto journalResult = PowerResilience::CreateWriteAheadJournal(std::move(journalFile.Value()));
        EXPECT_TRUE(journalResult.IsOk());
        rig.journal = std::move(journalResult.Value());

        auto backingResult = Storage::OpenFileBackingStore(PathFor(stem + ".store"));
        EXPECT_TRUE(backingResult.IsOk());
        rig.backingStore = std::move(backingResult.Value());

        auto engineResult = CoreEngine::CreateCacheEngine(options, rig.backingStore, rig.journal);
        EXPECT_TRUE(engineResult.IsOk());
        rig.engine = std::move(engineResult.Value());

        EXPECT_TRUE(rig.engine->MarkRecoveryComplete().IsOk());
        return rig;
    }

    fs::path testDir_;
};

} // namespace

// ---------------------------------------------------------------------
// Cache hit.
// ---------------------------------------------------------------------

TEST_F(CacheEngineCoreTest, Hit_ReturnsExactlyWhatWasInserted) {
    auto rig = BuildReadyRig("hit1");
    ASSERT_TRUE(rig.engine->Put("k1", Bytes("hello world")).IsOk());

    auto result = rig.engine->Get("k1");
    ASSERT_TRUE(result.IsOk());
    EXPECT_EQ(ToStr(result.Value()), "hello world");

    auto stats = rig.engine->GetStatistics();
    EXPECT_EQ(stats.hitCount, 1u);
    EXPECT_EQ(stats.missCount, 0u);
}

TEST_F(CacheEngineCoreTest, Hit_RepeatedGetsAllCountAsHits) {
    auto rig = BuildReadyRig("hit2");
    ASSERT_TRUE(rig.engine->Put("k1", Bytes("v")).IsOk());

    for (int i = 0; i < 5; ++i) {
        auto result = rig.engine->Get("k1");
        ASSERT_TRUE(result.IsOk());
    }

    EXPECT_EQ(rig.engine->GetStatistics().hitCount, 5u);
}

// ---------------------------------------------------------------------
// Cache miss.
// ---------------------------------------------------------------------

TEST_F(CacheEngineCoreTest, Miss_KeyAbsentEverywhere_ReturnsExplicitNotFound) {
    auto rig = BuildReadyRig("miss1");

    auto result = rig.engine->Get("nope");
    ASSERT_FALSE(result.IsOk());
    EXPECT_EQ(result.Err().code, Common::ErrorCode::NotFound);

    auto stats = rig.engine->GetStatistics();
    EXPECT_EQ(stats.missCount, 1u);
    EXPECT_EQ(stats.hitCount, 0u);
}

TEST_F(CacheEngineCoreTest, Miss_ThenHit_CountersReflectBothExactly) {
    auto rig = BuildReadyRig("miss2");

    auto missResult = rig.engine->Get("k");
    EXPECT_FALSE(missResult.IsOk());

    ASSERT_TRUE(rig.engine->Put("k", Bytes("now present")).IsOk());

    auto hitResult = rig.engine->Get("k");
    ASSERT_TRUE(hitResult.IsOk());

    auto stats = rig.engine->GetStatistics();
    EXPECT_EQ(stats.missCount, 1u);
    EXPECT_EQ(stats.hitCount, 1u);
}

// ---------------------------------------------------------------------
// Insertion.
// ---------------------------------------------------------------------

TEST_F(CacheEngineCoreTest, Insert_NewKey_IsCountedAsInsertNotUpdate) {
    auto rig = BuildReadyRig("insert1");
    ASSERT_TRUE(rig.engine->Put("brand-new", Bytes("data")).IsOk());

    auto stats = rig.engine->GetStatistics();
    EXPECT_EQ(stats.insertCount, 1u);
    EXPECT_EQ(stats.updateCount, 0u);
    EXPECT_EQ(stats.currentEntryCount, 1u);
}

TEST_F(CacheEngineCoreTest, Insert_MultipleDistinctKeys_AllRetrievable) {
    auto rig = BuildReadyRig("insert2");
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(rig.engine->Put("key" + std::to_string(i), Bytes("value" + std::to_string(i))).IsOk());
    }
    for (int i = 0; i < 10; ++i) {
        auto result = rig.engine->Get("key" + std::to_string(i));
        ASSERT_TRUE(result.IsOk());
        EXPECT_EQ(ToStr(result.Value()), "value" + std::to_string(i));
    }
    EXPECT_EQ(rig.engine->GetStatistics().insertCount, 10u);
    EXPECT_EQ(rig.engine->GetStatistics().currentEntryCount, 10u);
}

// ---------------------------------------------------------------------
// Update.
// ---------------------------------------------------------------------

TEST_F(CacheEngineCoreTest, Update_ExistingKey_ReplacesValueAndCountsAsUpdate) {
    auto rig = BuildReadyRig("update1");
    ASSERT_TRUE(rig.engine->Put("k", Bytes("first")).IsOk());
    ASSERT_TRUE(rig.engine->Put("k", Bytes("second")).IsOk());

    auto result = rig.engine->Get("k");
    ASSERT_TRUE(result.IsOk());
    EXPECT_EQ(ToStr(result.Value()), "second");

    auto stats = rig.engine->GetStatistics();
    EXPECT_EQ(stats.insertCount, 1u);
    EXPECT_EQ(stats.updateCount, 1u);
    EXPECT_EQ(stats.currentEntryCount, 1u); // still one logical entry, not two
}

TEST_F(CacheEngineCoreTest, Update_ChangesEntrySizeAccountingCorrectly) {
    auto rig = BuildReadyRig("update2");
    ASSERT_TRUE(rig.engine->Put("k", Bytes(std::string(10, 'a'))).IsOk());
    auto statsAfterSmall = rig.engine->GetStatistics();

    ASSERT_TRUE(rig.engine->Put("k", Bytes(std::string(1000, 'b'))).IsOk());
    auto statsAfterLarge = rig.engine->GetStatistics();

    // Memory accounting must reflect the larger value now, not the stale
    // smaller footprint from before the update.
    EXPECT_GT(statsAfterLarge.currentMemoryBytes, statsAfterSmall.currentMemoryBytes);
}

// ---------------------------------------------------------------------
// Invalidate / remove.
// ---------------------------------------------------------------------

TEST_F(CacheEngineCoreTest, Remove_ExistingCleanKey_MakesSubsequentGetAMiss) {
    auto rig = BuildReadyRig("remove1");
    ASSERT_TRUE(rig.engine->Put("k", Bytes("v")).IsOk()); // WriteThrough -> Clean immediately

    ASSERT_TRUE(rig.engine->Invalidate("k").IsOk());

    auto result = rig.engine->Get("k");
    EXPECT_FALSE(result.IsOk());
    EXPECT_EQ(result.Err().code, Common::ErrorCode::NotFound);
    EXPECT_EQ(rig.engine->GetStatistics().invalidationCount, 1u);
}

TEST_F(CacheEngineCoreTest, Remove_AlsoRemovesFromBackingStore) {
    auto rig = BuildReadyRig("remove2");
    ASSERT_TRUE(rig.engine->Put("k", Bytes("v")).IsOk());
    ASSERT_TRUE(rig.backingStore->Contains("k"));

    ASSERT_TRUE(rig.engine->Invalidate("k").IsOk());

    EXPECT_FALSE(rig.backingStore->Contains("k"));
}

TEST_F(CacheEngineCoreTest, Remove_NonExistentKey_DoesNotCrashAndReturnsExplicitResult) {
    auto rig = BuildReadyRig("remove3");
    // Removing a key that was never inserted must not silently succeed
    // as if data existed, nor crash; it must return a defined Result.
    auto result = rig.engine->Invalidate("never-existed");
    // Invalidate() on an absent key is well-defined success (nothing to
    // remove is not an error) — the important, tested guarantee is that
    // it is explicit and deterministic, never undefined behavior.
    EXPECT_TRUE(result.IsOk());
}

// ---------------------------------------------------------------------
// Capacity enforcement.
// ---------------------------------------------------------------------

TEST_F(CacheEngineCoreTest, Capacity_EntryCountLimit_IsEnforced) {
    CoreEngine::CacheEngineOptions options;
    options.shardCount = 1; // single shard => deterministic global LRU order
    options.maxEntryCount = 3;
    options.capacityBytes = 10ull * 1024 * 1024; // not the binding constraint here
    auto rig = BuildReadyRig("cap1", options);

    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(rig.engine->Put("k" + std::to_string(i), Bytes("v")).IsOk());
    }

    auto stats = rig.engine->GetStatistics();
    EXPECT_LE(stats.currentEntryCount, 3u)
        << "cache must never hold more than the configured maxEntryCount";
    EXPECT_GE(stats.evictionCount, 2u)
        << "inserting 5 keys with maxEntryCount=3 must have evicted at least 2";
}

TEST_F(CacheEngineCoreTest, Capacity_ByteBudget_IsEnforced) {
    CoreEngine::CacheEngineOptions options;
    options.shardCount = 1;
    options.maxEntryCount = 100000; // not the binding constraint
    options.capacityBytes = 500;    // small: forces byte-based eviction
    auto rig = BuildReadyRig("cap2", options);

    std::string value(100, 'x');
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(rig.engine->Put("bigkey" + std::to_string(i), Bytes(value)).IsOk());
    }

    auto stats = rig.engine->GetStatistics();
    EXPECT_LE(stats.currentMemoryBytes, options.capacityBytes + 400)
        << "memory usage must stay near the configured byte budget, not grow unbounded";
    EXPECT_GT(stats.evictionCount, 0u);
}

TEST_F(CacheEngineCoreTest, Capacity_RejectsZeroCapacityAtConstruction) {
    CoreEngine::CacheEngineOptions options;
    options.capacityBytes = 0;

    auto journalFile = Storage::OpenFile(PathFor("cap3.journal"), Storage::OpenMode::OpenOrCreate);
    auto journalResult = PowerResilience::CreateWriteAheadJournal(std::move(journalFile.Value()));
    std::shared_ptr<PowerResilience::IWriteAheadJournal> journal = std::move(journalResult.Value());
    auto backingResult = Storage::OpenFileBackingStore(PathFor("cap3.store"));
    std::shared_ptr<Storage::IBackingStore> backingStore = std::move(backingResult.Value());

    auto engineResult = CoreEngine::CreateCacheEngine(options, backingStore, journal);
    EXPECT_FALSE(engineResult.IsOk());
    EXPECT_EQ(engineResult.Err().code, Common::ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------
// Real LRU eviction (verifying LEAST recently used is what gets evicted,
// not an arbitrary or FIFO choice).
// ---------------------------------------------------------------------

TEST_F(CacheEngineCoreTest, Lru_LeastRecentlyUsedEntryIsEvictedFirst) {
    CoreEngine::CacheEngineOptions options;
    options.shardCount = 1;
    options.maxEntryCount = 3;
    options.capacityBytes = 10ull * 1024 * 1024;
    auto rig = BuildReadyRig("lru1", options);

    ASSERT_TRUE(rig.engine->Put("a", Bytes("1")).IsOk());
    ASSERT_TRUE(rig.engine->Put("b", Bytes("2")).IsOk());
    ASSERT_TRUE(rig.engine->Put("c", Bytes("3")).IsOk());
    // Access "a" so it becomes most-recently-used; "b" is now the least
    // recently used of {a, b, c}.
    ASSERT_TRUE(rig.engine->Get("a").IsOk());

    // Insert a 4th key: capacity is 3, so exactly one entry must be
    // evicted, and it must be "b" (the true LRU), not "a" or "c".
    ASSERT_TRUE(rig.engine->Put("d", Bytes("4")).IsOk());

    EXPECT_TRUE(rig.engine->Contains("a")) << "recently-accessed 'a' must survive eviction";
    EXPECT_TRUE(rig.engine->Contains("c")) << "'c' is newer than 'b' and must survive";
    EXPECT_TRUE(rig.engine->Contains("d")) << "just-inserted 'd' must be present";

    // "b" was least-recently-used at the time of the evicting insert and
    // must have been the one reclaimed — checked via the cache-only path
    // (GetEntryInfo) so a backing-store fallback can't hide an eviction.
    auto infoB = rig.engine->GetEntryInfo("b");
    EXPECT_FALSE(infoB.IsOk()) << "'b' (the true LRU victim) must have been evicted from the cache";
}

TEST_F(CacheEngineCoreTest, Lru_UpdatingAnEntryRefreshesItsRecency) {
    CoreEngine::CacheEngineOptions options;
    options.shardCount = 1;
    options.maxEntryCount = 2;
    options.capacityBytes = 10ull * 1024 * 1024;
    auto rig = BuildReadyRig("lru2", options);

    ASSERT_TRUE(rig.engine->Put("a", Bytes("1")).IsOk());
    ASSERT_TRUE(rig.engine->Put("b", Bytes("2")).IsOk());
    // Re-Put "a" (an update, not a fresh insert) — this must count as a
    // recency-refreshing touch, exactly like a Get would.
    ASSERT_TRUE(rig.engine->Put("a", Bytes("1-updated")).IsOk());

    // Now "b" is the LRU; inserting "c" must evict "b", not "a".
    ASSERT_TRUE(rig.engine->Put("c", Bytes("3")).IsOk());

    EXPECT_TRUE(rig.engine->GetEntryInfo("a").IsOk());
    EXPECT_FALSE(rig.engine->GetEntryInfo("b").IsOk());
    EXPECT_TRUE(rig.engine->GetEntryInfo("c").IsOk());
}

// ---------------------------------------------------------------------
// Memory accounting.
// ---------------------------------------------------------------------

TEST_F(CacheEngineCoreTest, MemoryAccounting_GrowsAndShrinksWithInsertAndRemove) {
    auto rig = BuildReadyRig("mem1");
    auto statsEmpty = rig.engine->GetStatistics();
    EXPECT_EQ(statsEmpty.currentMemoryBytes, 0u);

    ASSERT_TRUE(rig.engine->Put("k", Bytes(std::string(500, 'z'))).IsOk());
    auto statsAfterInsert = rig.engine->GetStatistics();
    EXPECT_GT(statsAfterInsert.currentMemoryBytes, 500u); // value bytes plus real bookkeeping overhead

    ASSERT_TRUE(rig.engine->Invalidate("k").IsOk());
    auto statsAfterRemove = rig.engine->GetStatistics();
    EXPECT_EQ(statsAfterRemove.currentMemoryBytes, 0u);
}

// ---------------------------------------------------------------------
// Explicit error handling.
// ---------------------------------------------------------------------

TEST_F(CacheEngineCoreTest, ErrorHandling_GetBeforeRecoveryComplete_ReturnsExplicitError) {
    auto journalFile = Storage::OpenFile(PathFor("err1.journal"), Storage::OpenMode::OpenOrCreate);
    auto journalResult = PowerResilience::CreateWriteAheadJournal(std::move(journalFile.Value()));
    std::shared_ptr<PowerResilience::IWriteAheadJournal> journal = std::move(journalResult.Value());
    auto backingResult = Storage::OpenFileBackingStore(PathFor("err1.store"));
    std::shared_ptr<Storage::IBackingStore> backingStore = std::move(backingResult.Value());

    CoreEngine::CacheEngineOptions options;
    options.writePolicy = CoreEngine::WritePolicyKind::WriteThrough;
    auto engineResult = CoreEngine::CreateCacheEngine(options, backingStore, journal);
    ASSERT_TRUE(engineResult.IsOk());
    auto engine = std::move(engineResult.Value());

    // MarkRecoveryComplete() deliberately not called yet.
    auto result = engine->Get("anything");
    ASSERT_FALSE(result.IsOk());
    EXPECT_EQ(result.Err().code, Common::ErrorCode::RecoveryNotComplete);
}

TEST_F(CacheEngineCoreTest, ErrorHandling_DisabledCache_ReturnsExplicitErrorNotSilentNoOp) {
    CoreEngine::CacheEngineOptions options;
    options.enabled = false;
    auto rig = BuildReadyRig("err2", options);

    auto getResult = rig.engine->Get("k");
    ASSERT_FALSE(getResult.IsOk());
    EXPECT_EQ(getResult.Err().code, Common::ErrorCode::CacheDisabled);

    auto putResult = rig.engine->Put("k", Bytes("v"));
    ASSERT_FALSE(putResult.IsOk());
    EXPECT_EQ(putResult.Err().code, Common::ErrorCode::CacheDisabled);
}

TEST_F(CacheEngineCoreTest, ErrorHandling_NeverSilentlySucceedsOnAMiss) {
    // A miss with nothing in the backing store either must be reported as
    // an explicit, typed error — never as a "successful" empty/default
    // value that a caller could mistake for real cached data.
    auto rig = BuildReadyRig("err3");
    auto result = rig.engine->Get("absent-everywhere");
    ASSERT_FALSE(result.IsOk());
    EXPECT_EQ(result.Err().code, Common::ErrorCode::NotFound);
    EXPECT_FALSE(result.Err().message.empty());
}

// ---------------------------------------------------------------------
// Thread-safe concurrent access (reads and writes).
// ---------------------------------------------------------------------

TEST_F(CacheEngineCoreTest, Concurrency_ManyThreadsInsertingDistinctKeys_NoLossNoCorruption) {
    auto rig = BuildReadyRig("conc1");

    constexpr int kThreads = 8;
    constexpr int kKeysPerThread = 250;
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kKeysPerThread; ++i) {
                std::string key = "t" + std::to_string(t) + "_" + std::to_string(i);
                ASSERT_TRUE(rig.engine->Put(key, Bytes("val" + std::to_string(i))).IsOk());
            }
        });
    }
    for (auto& th : threads) th.join();

    int verified = 0;
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kKeysPerThread; ++i) {
            std::string key = "t" + std::to_string(t) + "_" + std::to_string(i);
            auto result = rig.engine->Get(key);
            ASSERT_TRUE(result.IsOk()) << "lost key under concurrent insert: " << key;
            EXPECT_EQ(ToStr(result.Value()), "val" + std::to_string(i));
            ++verified;
        }
    }
    EXPECT_EQ(verified, kThreads * kKeysPerThread);
}

TEST_F(CacheEngineCoreTest, Concurrency_ReadersAndWritersOnSharedKeys_NoTornOrCorruptValues) {
    auto rig = BuildReadyRig("conc2");
    ASSERT_TRUE(rig.engine->Put("shared", Bytes("initial")).IsOk());

    std::atomic<bool> stop{false};
    std::atomic<int> corruptReads{0};
    std::atomic<int> writeErrors{0};

    std::thread writer([&]() {
        for (int i = 0; i < 2000 && !stop.load(); ++i) {
            std::string value = "w" + std::to_string(i);
            if (!rig.engine->Put("shared", Bytes(value)).IsOk()) {
                writeErrors.fetch_add(1);
            }
        }
        stop.store(true);
    });

    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&]() {
            while (!stop.load()) {
                auto result = rig.engine->Get("shared");
                if (result.IsOk()) {
                    std::string value = ToStr(result.Value());
                    // Must always be either the initial value or a
                    // complete, well-formed "wN" write — never a partial
                    // or mixed byte sequence from two concurrent writers.
                    bool wellFormed = (value == "initial") || (value.size() >= 2 && value[0] == 'w');
                    if (!wellFormed) corruptReads.fetch_add(1);
                }
            }
        });
    }

    writer.join();
    for (auto& r : readers) r.join();

    EXPECT_EQ(corruptReads.load(), 0);
    EXPECT_EQ(writeErrors.load(), 0);
}

TEST_F(CacheEngineCoreTest, Concurrency_ConcurrentInvalidateAndGet_NoUseAfterFreeOrCrash) {
    auto rig = BuildReadyRig("conc3");
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(rig.engine->Put("k" + std::to_string(i), Bytes("v")).IsOk());
    }

    std::atomic<int> unexpectedErrors{0};

    std::thread invalidator([&]() {
        for (int i = 0; i < 100; ++i) {
            auto result = rig.engine->Invalidate("k" + std::to_string(i));
            if (!result.IsOk()) unexpectedErrors.fetch_add(1);
        }
    });

    std::thread reader([&]() {
        for (int round = 0; round < 5; ++round) {
            for (int i = 0; i < 100; ++i) {
                // Either NotFound (already invalidated) or a successful
                // read of the still-present value are both valid outcomes
                // here; anything else (a crash, UB, or a different error
                // code) would fail the test.
                auto result = rig.engine->Get("k" + std::to_string(i));
                if (!result.IsOk() && result.Err().code != Common::ErrorCode::NotFound) {
                    unexpectedErrors.fetch_add(1);
                }
            }
        }
    });

    invalidator.join();
    reader.join();

    EXPECT_EQ(unexpectedErrors.load(), 0);
}

TEST_F(CacheEngineCoreTest, Concurrency_StatisticsCountersAreConsistentUnderLoad) {
    auto rig = BuildReadyRig("conc4");

    constexpr int kThreads = 6;
    constexpr int kOpsPerThread = 200;
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kOpsPerThread; ++i) {
                std::string key = "ctr_t" + std::to_string(t) + "_" + std::to_string(i);
                ASSERT_TRUE(rig.engine->Put(key, Bytes("v")).IsOk());
                ASSERT_TRUE(rig.engine->Get(key).IsOk());
            }
        });
    }
    for (auto& th : threads) th.join();

    auto stats = rig.engine->GetStatistics();
    // Every Put() above was a fresh, distinct key => every one must have
    // been counted as an insert, and every Get() immediately following it
    // must have been a hit. Counters are real per-operation increments,
    // not estimates, so these must match exactly.
    EXPECT_EQ(stats.insertCount, static_cast<std::uint64_t>(kThreads * kOpsPerThread));
    EXPECT_EQ(stats.hitCount, static_cast<std::uint64_t>(kThreads * kOpsPerThread));
}
