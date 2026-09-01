// Deterministic and stress reproduction of the audited read-miss/write
// race: Get(key) misses, releases the shard lock, and issues an
// UNLOCKED backing-store read; while that read is in flight, another
// thread performs Put(key, newValue) which durably journals the write
// and commits a newer, Dirty in-memory entry. The original Get's
// backing-store read then completes with the OLDER value. The stale
// read result must never overwrite, nor be returned instead of, the
// newer dirty cache entry.
//
// See src/CoreEngine/src/CacheEngine.cpp (file-header "Read-fill
// linearization" comment, and the extended comment directly above
// CacheEngine::Get()) for the full description of the bug that was
// found and the two-part fix (per-shard mutation fence + reserved
// sentinel version 0 for read-fills). This file is the regression
// coverage for that fix.
//
// The deterministic test below does NOT rely on thread scheduling luck:
// it uses a real IBackingStore decorator (DelayableBackingStore) that
// blocks its Get() call on a real std::condition_variable until the
// test explicitly releases it, so the race window is opened and closed
// under full test control, on every run, on every machine. The stress
// test afterward additionally hammers the race under normal scheduling
// across many iterations/threads to catch anything the deterministic
// test's specific interleaving might miss.
#include "QuantumCache/CoreEngine/ICacheEngine.h"
#include "QuantumCache/PowerResilience/IWriteAheadJournal.h"
#include "QuantumCache/Storage/IBackingStore.h"
#include "QuantumCache/Storage/IFile.h"
#include <gtest/gtest.h>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
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

// Wraps a real Storage::IBackingStore (FileBackingStore, backed by a
// real file — never a mock of storage semantics) and adds the ability
// to pause a specific Get() call mid-flight, deterministically, under
// test control. Every method not explicitly overridden here forwards
// unchanged to the real implementation, so all durability/CRC/
// crash-consistency behavior remains exactly the production code path.
class DelayableBackingStore final : public Storage::IBackingStore {
public:
    explicit DelayableBackingStore(std::shared_ptr<Storage::IBackingStore> inner)
        : inner_(std::move(inner)) {}

    // Arms a one-shot delay: the NEXT Get() call for `key` will block
    // (after having already been dispatched to the inner store, so the
    // "read is in flight" state is real) until ReleaseGet() is called
    // from another thread. Also signals `armedCv_`/`armed_` so the
    // arming thread can deterministically wait until the delayed Get()
    // has actually entered its blocked state before proceeding — this
    // is what makes the whole test deterministic rather than a sleep()
    // guess.
    void ArmDelayForNextGet(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        delayedKey_ = key;
        delayArmed_ = true;
        released_ = false;
        enteredDelay_ = false;
    }

    // Blocks until the delayed Get() call has actually entered its wait
    // (i.e. the backing-store read has genuinely started and is
    // suspended), so the caller can safely perform the concurrent
    // Put() knowing the race window is truly open.
    void WaitUntilGetIsBlocked() {
        std::unique_lock<std::mutex> lock(mutex_);
        enteredCv_.wait(lock, [this]() { return enteredDelay_; });
    }

    void ReleaseGet() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        releaseCv_.notify_all();
    }

    Common::Result<std::vector<std::uint8_t>> Get(const std::string& key) override {
        bool shouldDelay = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (delayArmed_ && key == delayedKey_) {
                shouldDelay = true;
                delayArmed_ = false; // one-shot
            }
        }

        // Real I/O against the real backing store happens BEFORE we
        // block, exactly mirroring "the read is genuinely in flight
        // when the race window opens" (in production, the delay would
        // be actual disk latency; here real reads still occur, we just
        // additionally hold the result before returning it).
        auto result = inner_->Get(key);

        if (shouldDelay) {
            std::unique_lock<std::mutex> lock(mutex_);
            enteredDelay_ = true;
            enteredCv_.notify_all();
            releaseCv_.wait(lock, [this]() { return released_; });
        }

        return result;
    }

    Common::Result<void> Put(const std::string& key, const std::vector<std::uint8_t>& value) override {
        return inner_->Put(key, value);
    }

    Common::Result<void> PutBatch(const std::vector<Storage::BackingStoreRecord>& records) override {
        return inner_->PutBatch(records);
    }

    Common::Result<void> Remove(const std::string& key) override { return inner_->Remove(key); }

    bool Contains(const std::string& key) override { return inner_->Contains(key); }

    std::size_t EntryCount() const noexcept override { return inner_->EntryCount(); }

    std::uint64_t GetVersion(const std::string& key) override { return inner_->GetVersion(key); }

private:
    std::shared_ptr<Storage::IBackingStore> inner_;

    std::mutex mutex_;
    std::condition_variable enteredCv_;
    std::condition_variable releaseCv_;
    std::string delayedKey_;
    bool delayArmed_{false};
    bool enteredDelay_{false};
    bool released_{false};
};

class CacheEngineReadWriteRaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        testDir_ = fs::temp_directory_path() /
                   ("qc_race_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                    "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
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

    struct Rig {
        std::shared_ptr<PowerResilience::IWriteAheadJournal> journal;
        std::shared_ptr<DelayableBackingStore> delayableStore;
        std::unique_ptr<CoreEngine::ICacheEngine> engine;
    };

    Rig BuildReadyRig(const std::string& stem, CoreEngine::CacheEngineOptions options = {}) {
        Rig rig;

        auto journalFile = Storage::OpenFile(PathFor(stem + ".journal"), Storage::OpenMode::OpenOrCreate);
        EXPECT_TRUE(journalFile.IsOk());
        auto journalResult = PowerResilience::CreateWriteAheadJournal(std::move(journalFile.Value()));
        EXPECT_TRUE(journalResult.IsOk());
        rig.journal = std::move(journalResult.Value());

        auto realBackingResult = Storage::OpenFileBackingStore(PathFor(stem + ".store"));
        EXPECT_TRUE(realBackingResult.IsOk());
        std::shared_ptr<Storage::IBackingStore> realBackingStore = std::move(realBackingResult.Value());
        rig.delayableStore = std::make_shared<DelayableBackingStore>(realBackingStore);

        auto engineResult = CoreEngine::CreateCacheEngine(options, rig.delayableStore, rig.journal);
        EXPECT_TRUE(engineResult.IsOk());
        rig.engine = std::move(engineResult.Value());
        EXPECT_TRUE(rig.engine->MarkRecoveryComplete().IsOk());

        return rig;
    }

    fs::path testDir_;
};

} // namespace

// ---------------------------------------------------------------------
// Deterministic reproduction.
// ---------------------------------------------------------------------

TEST_F(CacheEngineReadWriteRaceTest,
       StaleReadFill_NeverOverwritesNewerDirtyEntry_DeterministicInterleaving) {
    auto rig = BuildReadyRig("det1");

    // Seed the backing store with the OLD value, entirely outside the
    // cache, exactly like data that predates the cache being warmed.
    // (Direct backing-store access via the real inner store the
    // decorator wraps, not through the engine, so this setup step
    // itself cannot be the thing racing.)
    ASSERT_TRUE(rig.engine->GetStatistics().currentEntryCount == 0);
    {
        // Populate directly via the engine once, then invalidate, so the
        // backing store ends up holding "old-value" with the cache
        // empty, without depending on internal DelayableBackingStore
        // wiring order.
    }
    // Simplest reliable seeding: use the engine's own WriteThrough path
    // once via a throwaway options-free Put would leave it Dirty; instead
    // seed the backing store directly through the decorator's Put(),
    // which forwards to the real store.
    ASSERT_TRUE(rig.delayableStore->Put("racer-key", Bytes("old-value")).IsOk());

    // Arm the race: the NEXT Get("racer-key") will block, mid-flight,
    // after actually reading "old-value" from the real backing store,
    // until explicitly released.
    rig.delayableStore->ArmDelayForNextGet("racer-key");

    std::atomic<bool> readerDone{false};
    Common::Result<std::vector<std::uint8_t>> readerResult =
        Common::Result<std::vector<std::uint8_t>>::Failure(
            Common::Error{Common::ErrorCode::Unknown, "reader thread did not complete", 0});

    std::thread reader([&]() {
        readerResult = rig.engine->Get("racer-key");
        readerDone.store(true);
    });

    // Wait until the reader's backing-store read has genuinely started
    // and is blocked — this is the deterministic replacement for a
    // sleep()-based race window.
    rig.delayableStore->WaitUntilGetIsBlocked();

    // Now, while the stale read is provably in flight, perform the
    // concurrent write. This is a REAL Put(): it durably journals
    // "new-value" and commits a Dirty entry to the live shard.
    ASSERT_TRUE(rig.engine->Put("racer-key", Bytes("new-value")).IsOk());

    // Confirm the write landed as a live, Dirty entry BEFORE releasing
    // the stale reader, so we know for certain the race window captured
    // exactly the scenario under test.
    auto infoAfterWrite = rig.engine->GetEntryInfo("racer-key");
    ASSERT_TRUE(infoAfterWrite.IsOk());
    EXPECT_EQ(infoAfterWrite.Value().dirtyState, CoreEngine::EntryDirtyState::Dirty);

    // Release the stale read and let it complete.
    rig.delayableStore->ReleaseGet();
    reader.join();
    ASSERT_TRUE(readerDone.load());

    // The stale Get() call itself must still complete successfully (it
    // performed a valid, if momentarily-outdated, read) — but per the
    // requirement, it must never be returned INSTEAD OF the newer dirty
    // entry now that a newer value is known to exist.
    ASSERT_TRUE(readerResult.IsOk());
    EXPECT_EQ(ToStr(readerResult.Value()), "new-value")
        << "Get() returned the STALE backing-store value instead of the newer, "
           "already-committed Dirty entry — this is exactly the audited race.";

    // The live cache entry itself must still hold the newer value and
    // must still be Dirty — the stale read must not have overwritten it
    // (checked independently of what Get() happened to return, so this
    // assertion catches the "overwrite" half of the bug even if some
    // other code path masked the "return" half).
    auto infoAfterRace = rig.engine->GetEntryInfo("racer-key");
    ASSERT_TRUE(infoAfterRace.IsOk());
    EXPECT_EQ(infoAfterRace.Value().dirtyState, CoreEngine::EntryDirtyState::Dirty)
        << "the newer entry's Dirty flag was clobbered by the stale read-fill";

    // And a fresh Get() (a plain cache hit this time, no race involved)
    // must also see the new value, confirming the cache's live state is
    // actually correct, not just Get()'s one-off return value.
    auto followUpGet = rig.engine->Get("racer-key");
    ASSERT_TRUE(followUpGet.IsOk());
    EXPECT_EQ(ToStr(followUpGet.Value()), "new-value");

    // The write must never have been lost/never-journaled either: still
    // flushable, and flushing it must persist "new-value", not
    // "old-value", to the backing store.
    ASSERT_TRUE(rig.engine->Flush("racer-key").IsOk());
    auto backingAfterFlush = rig.delayableStore->Get("racer-key");
    ASSERT_TRUE(backingAfterFlush.IsOk());
    EXPECT_EQ(ToStr(backingAfterFlush.Value()), "new-value");
}

TEST_F(CacheEngineReadWriteRaceTest,
       StaleReadFill_DiscardedWithoutError_WhenKeyWasInvalidatedThroughEngineDuringRead) {
    // Related linearization scenario explicitly covered by the same
    // fence mechanism: Get() misses, backing-store read is in flight,
    // and the racing operation is a full ForceInvalidate/removal made
    // THROUGH THE ENGINE (not a direct out-of-band mutation of the
    // backing store, which is a fundamentally different and
    // out-of-scope hazard no caching layer can transparently detect
    // without polling — the engine's consistency guarantees only cover
    // mutations made via its own API, exactly like every write-back
    // cache). The stale read must not resurrect the removed key by
    // inserting itself into the cache as if the key were still present
    // and clean.
    auto rig = BuildReadyRig("det2");
    ASSERT_TRUE(rig.delayableStore->Put("doomed-key", Bytes("about-to-be-removed")).IsOk());

    // Arm a delay on the very first read-fill for this key.
    rig.delayableStore->ArmDelayForNextGet("doomed-key");

    std::atomic<bool> readerDone{false};
    Common::Result<std::vector<std::uint8_t>> readerResult =
        Common::Result<std::vector<std::uint8_t>>::Failure(
            Common::Error{Common::ErrorCode::Unknown, "reader thread did not complete", 0});
    std::thread reader([&]() {
        readerResult = rig.engine->Get("doomed-key");
        readerDone.store(true);
    });

    rig.delayableStore->WaitUntilGetIsBlocked();

    // While the stale read is in flight (it already observed
    // "about-to-be-removed" from the backing store, but has not yet
    // committed anything to the cache), authoritatively remove the key
    // THROUGH THE ENGINE. Since nothing is cached yet, only
    // ForceInvalidate (not the plain, Clean-only Invalidate) applies;
    // InvalidateImpl's early snapshot will see "not present in cache"
    // (entryVersion 0) and still journal+backing-store-remove the key,
    // and — the specific behavior under test — bump the shard's
    // mutation fence so the in-flight read-fill is forced to discard
    // its now-stale result.
    auto invalidateResult = rig.engine->ForceInvalidate("doomed-key");
    ASSERT_TRUE(invalidateResult.IsOk());
    ASSERT_FALSE(rig.delayableStore->Contains("doomed-key"));

    rig.delayableStore->ReleaseGet();
    reader.join();
    ASSERT_TRUE(readerDone.load());

    // The stale read's own Result is a historical fact (it really did
    // read that value at some point, before the concurrent removal) and
    // is allowed to succeed, but it must NOT have been cached — the live
    // cache must reflect the truly current, authoritative state
    // (absent), not resurrect stale data.
    auto info = rig.engine->GetEntryInfo("doomed-key");
    EXPECT_FALSE(info.IsOk())
        << "stale read-fill resurrected a key that was concurrently, authoritatively "
           "removed through the engine's own ForceInvalidate() while the read was in flight";

    // A follow-up Get() must correctly report NotFound (in both cache
    // and backing store), not silently serve the stale value either.
    auto followUp = rig.engine->Get("doomed-key");
    EXPECT_FALSE(followUp.IsOk());
    if (!followUp.IsOk()) {
        EXPECT_EQ(followUp.Err().code, Common::ErrorCode::NotFound);
    }
}

// ---------------------------------------------------------------------
// Stress reproduction (real thread scheduling, many iterations).
// ---------------------------------------------------------------------

TEST_F(CacheEngineReadWriteRaceTest,
       StressManyRacingReadersAndWriters_CacheNeverServesOrRetainsStaleValueOverNewer) {
    auto rig = BuildReadyRig("stress1");

    constexpr int kKeys = 20;
    constexpr int kIterationsPerKey = 300;

    for (int k = 0; k < kKeys; ++k) {
        ASSERT_TRUE(rig.delayableStore->Put("stresskey" + std::to_string(k), Bytes("v0")).IsOk());
    }

    std::atomic<bool> stop{false};
    std::atomic<int> staleReadObserved{0};
    std::atomic<int> unexpectedErrors{0};

    // Writers: continuously bump each key's value to a strictly
    // increasing, self-describing sequence number so a reader can tell
    // whether the value it observed is at least as new as the highest
    // value known to have been committed at any point "close enough" to
    // when the read happened. The real correctness invariant checked
    // below does not rely on wall-clock timing at all, though: it
    // re-reads the live cache/backing-store state AFTER each racy read
    // and asserts the cache's own dirty entry (if any) is never older
    // than a value the read itself observed - i.e. the read must never
    // have clobbered something newer than what it saw.
    std::vector<std::thread> writers;
    for (int k = 0; k < kKeys; ++k) {
        writers.emplace_back([&, k]() {
            std::string key = "stresskey" + std::to_string(k);
            for (int i = 1; i <= kIterationsPerKey && !stop.load(); ++i) {
                ASSERT_TRUE(rig.engine->Put(key, Bytes("v" + std::to_string(i))).IsOk());
            }
        });
    }

    std::vector<std::thread> readers;
    for (int r = 0; r < 6; ++r) {
        readers.emplace_back([&]() {
            for (int i = 0; i < kIterationsPerKey * kKeys / 6 && !stop.load(); ++i) {
                int k = i % kKeys;
                std::string key = "stresskey" + std::to_string(k);

                auto readResult = rig.engine->Get(key);
                if (!readResult.IsOk()) {
                    unexpectedErrors.fetch_add(1);
                    continue;
                }
                std::string observedValue = ToStr(readResult.Value());
                // Must be a well-formed "vN" value, never a torn or
                // empty read.
                if (observedValue.empty() || observedValue[0] != 'v') {
                    unexpectedErrors.fetch_add(1);
                    continue;
                }
                int observedN = std::atoi(observedValue.c_str() + 1);

                // Core invariant under test: whatever is CURRENTLY live
                // in the cache for this key (if anything) must never be
                // older (lower N) than what THIS read itself already
                // observed — if it were older, that would mean this
                // read's own (possibly stale) fill overwrote something
                // that used to be at least as new as what the read saw,
                // which is exactly the forbidden "stale overwrites
                // newer" outcome. (A DIFFERENT, even-newer write racing
                // in in the meantime is fine and expected — only going
                // backwards relative to what this read itself observed
                // is forbidden.)
                auto info = rig.engine->GetEntryInfo(key);
                if (info.IsOk()) {
                    std::string liveValue;
                    // GetEntryInfo does not expose the value; use a
                    // hit-path Get() (no artificial delay armed at this
                    // point, so this is a plain, race-free read for
                    // verification purposes) to inspect current state.
                    auto liveRead = rig.engine->Get(key);
                    if (liveRead.IsOk()) {
                        liveValue = ToStr(liveRead.Value());
                        if (!liveValue.empty() && liveValue[0] == 'v') {
                            int liveN = std::atoi(liveValue.c_str() + 1);
                            if (liveN < observedN) {
                                staleReadObserved.fetch_add(1);
                            }
                        }
                    }
                }
            }
        });
    }

    for (auto& w : writers) w.join();
    stop.store(true);
    for (auto& r : readers) r.join();

    EXPECT_EQ(staleReadObserved.load(), 0)
        << "detected at least one case where the live cache regressed to a value "
           "older than one a concurrent read had already observed — stale "
           "read-fill overwrote newer state";
    EXPECT_EQ(unexpectedErrors.load(), 0);
}

TEST_F(CacheEngineReadWriteRaceTest,
       StressReadInvalidateRace_NeverResurrectsStaleValueAfterAuthoritativeRemoval) {
    auto rig = BuildReadyRig("stress2");

    constexpr int kKeys = 10;
    constexpr int kIterations = 200;

    std::atomic<int> resurrections{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> threads;
    for (int k = 0; k < kKeys; ++k) {
        threads.emplace_back([&, k]() {
            std::string key = "invkey" + std::to_string(k);
            for (int i = 0; i < kIterations && !stop.load(); ++i) {
                // Cycle: Put (Dirty) -> Flush (Clean) -> ForceInvalidate
                // (removed) -> repeat, with a concurrent Get() racing
                // throughout from a second thread below.
                if (!rig.engine->Put(key, Bytes("cycleval")).IsOk()) continue;
                (void)rig.engine->Flush(key);
                (void)rig.engine->ForceInvalidate(key);
            }
        });
    }

    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&]() {
            for (int i = 0; i < kIterations * kKeys / 4 && !stop.load(); ++i) {
                int k = i % kKeys;
                std::string key = "invkey" + std::to_string(k);
                (void)rig.engine->Get(key); // outcome (hit/miss/NotFound) all legal
            }
        });
    }

    for (auto& t : threads) t.join();
    stop.store(true);
    for (auto& r : readers) r.join();

    // Final-state check: for every key, whatever the cache/backing store
    // currently agree on must be internally self-consistent — i.e. if
    // the backing store has genuinely forgotten the key (ForceInvalidate
    // removed it there too), the cache must not still be independently
    // serving a live entry for it as if it were valid, cached data.
    for (int k = 0; k < kKeys; ++k) {
        std::string key = "invkey" + std::to_string(k);
        bool backingHasIt = rig.delayableStore->Contains(key);
        auto info = rig.engine->GetEntryInfo(key);
        if (!backingHasIt && info.IsOk() && info.Value().dirtyState == CoreEngine::EntryDirtyState::Clean) {
            // A Clean cache entry claims to mirror the backing store
            // exactly; if the backing store has no record of the key at
            // all, a Clean entry for it is a genuine inconsistency.
            resurrections.fetch_add(1);
        }
    }

    EXPECT_EQ(resurrections.load(), 0)
        << "found a Clean cache entry for a key the backing store has no record of — "
           "a stale read-fill likely resurrected removed data";
}
