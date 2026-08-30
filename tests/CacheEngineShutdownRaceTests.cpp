// Deterministic reproduction and regression coverage for the AUDITED BUG
// found during Stage 2 hardening: Shutdown() could truncate the
// write-ahead journal while a concurrent Put()/Invalidate() had already
// durably appended its journal record but had not yet committed to
// in-memory shard state — silently destroying an already-acknowledged
// write. See CacheEngine.cpp's TryAdmitWrite()/ReleaseWriteAdmission()/
// DrainInFlightWrites() comment for the full interleaving and the fix
// (an explicit write-admission counter Shutdown() drains before deciding
// whether the journal is safe to truncate).
//
// This file uses a real IWriteAheadJournal decorator (DelayableJournal)
// that pauses a Put() call's Append() mid-flight, under full test
// control (a real std::condition_variable, not a sleep() guess), so the
// race window is opened and closed deterministically on every run.
#include "QuantumCache/CoreEngine/ICacheEngine.h"
#include "QuantumCache/PowerResilience/IWriteAheadJournal.h"
#include "QuantumCache/Storage/IBackingStore.h"
#include "QuantumCache/Storage/IFile.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
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

// Wraps a real IWriteAheadJournal (backed by a real file, held via
// shared_ptr since CreateCacheEngine takes a shared_ptr<IWriteAheadJournal>)
// and adds the ability to pause AFTER Append() has already durably
// written+flushed the record to disk, but BEFORE returning control to
// the caller (i.e. before CacheEngine::Put() proceeds to its shard-lock
// commit step). This exactly reproduces "the record is durably on disk,
// but the writer has not yet updated in-memory shard bookkeeping" — the
// precise window the audited bug exploited.
class DelayableJournal final : public PowerResilience::IWriteAheadJournal {
public:
    explicit DelayableJournal(std::shared_ptr<PowerResilience::IWriteAheadJournal> inner)
        : inner_(std::move(inner)) {}

    void ArmDelayForNextAppend() {
        std::lock_guard<std::mutex> lock(mutex_);
        delayArmed_ = true;
        released_ = false;
        enteredDelay_ = false;
    }

    // Blocks the calling (test) thread until the delayed Append() call
    // has actually entered its paused state — deterministic, no sleep().
    void WaitUntilAppendIsPaused() {
        std::unique_lock<std::mutex> lock(mutex_);
        enteredCv_.wait(lock, [this]() { return enteredDelay_; });
    }

    void ReleaseAppend() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        releaseCv_.notify_all();
    }

    Common::Result<std::uint64_t> Append(const std::vector<std::uint8_t>& payload) override {
        // The real durable append happens BEFORE we pause — the record
        // is genuinely on disk once this call is paused, exactly
        // mirroring the production interleaving under test.
        auto result = inner_->Append(payload);

        bool shouldDelay = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (delayArmed_) {
                shouldDelay = true;
                delayArmed_ = false;
            }
        }

        if (shouldDelay) {
            std::unique_lock<std::mutex> lock(mutex_);
            enteredDelay_ = true;
            enteredCv_.notify_all();
            releaseCv_.wait(lock, [this]() { return released_; });
        }

        return result;
    }

    Common::Result<void> Replay(const PowerResilience::ReplayCallback& callback) override {
        return inner_->Replay(callback);
    }
    Common::Result<void> Truncate() override { return inner_->Truncate(); }
    std::size_t RecordCount() const noexcept override { return inner_->RecordCount(); }

private:
    std::shared_ptr<PowerResilience::IWriteAheadJournal> inner_;
    std::mutex mutex_;
    std::condition_variable enteredCv_;
    std::condition_variable releaseCv_;
    bool delayArmed_{false};
    bool enteredDelay_{false};
    bool released_{false};
};

class CacheEngineShutdownRaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        testDir_ = fs::temp_directory_path() /
                   ("qc_shutdown_race_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
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

    fs::path testDir_;
};

} // namespace

TEST_F(CacheEngineShutdownRaceTest,
       ShutdownRacingPut_NeverTruncatesJournalBeforeInFlightWriteCommits) {
    auto journalFile = Storage::OpenFile(PathFor("race.journal"), Storage::OpenMode::OpenOrCreate);
    ASSERT_TRUE(journalFile.IsOk());
    auto realJournalResult = PowerResilience::CreateWriteAheadJournal(std::move(journalFile.Value()));
    ASSERT_TRUE(realJournalResult.IsOk());
    std::shared_ptr<PowerResilience::IWriteAheadJournal> realJournal =
        std::move(realJournalResult.Value());

    auto delayable = std::make_shared<DelayableJournal>(realJournal);

    auto backingResult = Storage::OpenFileBackingStore(PathFor("race.store"));
    ASSERT_TRUE(backingResult.IsOk());
    std::shared_ptr<Storage::IBackingStore> backingStore = std::move(backingResult.Value());

    auto engineResult = CoreEngine::CreateCacheEngine(
        CoreEngine::CacheEngineOptions{}, backingStore, delayable);
    ASSERT_TRUE(engineResult.IsOk());
    auto engine = std::move(engineResult.Value());
    ASSERT_TRUE(engine->MarkRecoveryComplete().IsOk());

    delayable->ArmDelayForNextAppend();

    std::atomic<bool> putReturned{false};
    Common::Result<void> putResult = Common::Result<void>::Success();
    std::thread putThread([&]() {
        putResult = engine->Put("racer", Bytes("must-not-be-lost"));
        putReturned.store(true);
    });

    // Wait until the Put()'s Append() call is genuinely paused (record
    // already durably on disk, shard state not yet updated) before
    // starting Shutdown() from this thread.
    delayable->WaitUntilAppendIsPaused();

    std::thread shutdownThread([&]() { (void)engine->Shutdown(); });

    // Give Shutdown() a brief real chance to run ahead if the fix were
    // absent (it would previously reach the truncate decision almost
    // immediately, well before we release the Put()). This sleep is not
    // what makes the test deterministic — DrainInFlightWrites() blocking
    // Shutdown() is what does — but it maximizes the chance of catching
    // a regression on a slow/loaded machine too.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(putReturned.load())
        << "Put() must still be blocked in Append() at this point (delay not yet released)";

    delayable->ReleaseAppend();

    putThread.join();
    shutdownThread.join();

    ASSERT_TRUE(putResult.IsOk()) << "the racing Put() must still succeed";

    // The critical safety property: the acknowledged write must be
    // durably present — either still in the (now-unflushed, since
    // Shutdown()'s FlushAll() ran before this Put() committed) journal,
    // or already folded into the backing store. It must NEVER be the
    // case that the journal was truncated out from under this write
    // (record count 0) while the backing store also does not have it.
    bool inBackingStore = backingStore->Contains("racer");
    bool journalHasRecords = realJournal->RecordCount() > 0;
    EXPECT_TRUE(inBackingStore || journalHasRecords)
        << "the racer key's write was acknowledged (Put() returned Ok) but is present in "
           "NEITHER the backing store nor the journal — this is exactly the data-loss bug "
           "the write-admission drain fixes";
}

TEST_F(CacheEngineShutdownRaceTest,
       ShutdownRacingPut_SurvivesSimulatedCrashAfterRace_DataFullyRecoverable) {
    // Stronger end-to-end version: after the race above resolves,
    // simulate a crash (destroy the engine without a second clean
    // Shutdown()) and verify a freshly reconstructed engine can recover
    // the racer key — proving the durability guarantee held all the way
    // through, not merely that Get()/Contains() looked right against
    // still-warm state.
    //
    // AUDITED FINDING (real Windows-runtime behavior, discovered via
    // Wine execution — see docs/ENVIRONMENT.md "Windows runtime
    // testing"): every IFile/IBackingStore handle opened in this first
    // phase (journalFile/realJournal, backingResult/backingStore) MUST
    // go out of scope (closing their underlying Win32 handles) before
    // the "reconstruct fresh" phase below opens NEW handles to the same
    // paths — Win32File opens with only FILE_SHARE_READ (correct,
    // intentional single-writer semantics), so overlapping handles to
    // the same file fail with a real Windows sharing violation. This is
    // a test-authoring requirement (matches every production call
    // site's restart-simulation pattern), not a product bug.
    {
        auto journalFile = Storage::OpenFile(PathFor("race2.journal"), Storage::OpenMode::OpenOrCreate);
        ASSERT_TRUE(journalFile.IsOk());
        auto realJournalResult = PowerResilience::CreateWriteAheadJournal(std::move(journalFile.Value()));
        std::shared_ptr<PowerResilience::IWriteAheadJournal> realJournal =
            std::move(realJournalResult.Value());
        auto delayable = std::make_shared<DelayableJournal>(realJournal);

        auto backingResult = Storage::OpenFileBackingStore(PathFor("race2.store"));
        std::shared_ptr<Storage::IBackingStore> backingStore = std::move(backingResult.Value());

        auto engineResult = CoreEngine::CreateCacheEngine(
            CoreEngine::CacheEngineOptions{}, backingStore, delayable);
        auto engine = std::move(engineResult.Value());
        ASSERT_TRUE(engine->MarkRecoveryComplete().IsOk());

        delayable->ArmDelayForNextAppend();

        std::thread putThread([&]() {
            (void)engine->Put("racer2", Bytes("also-must-not-be-lost"));
        });
        delayable->WaitUntilAppendIsPaused();

        std::thread shutdownThread([&]() { (void)engine->Shutdown(); });
        delayable->ReleaseAppend();

        putThread.join();
        shutdownThread.join();
        // engine (and journalFile/realJournal/backingResult/
        // backingStore, all scoped to this block) destroyed here; no
        // second explicit clean-shutdown marker step beyond what
        // Shutdown() itself already did.
    }

    // Reconstruct fresh (a real restart), replay, and confirm recovery.
    auto journalFile2 = Storage::OpenFile(PathFor("race2.journal"), Storage::OpenMode::OpenExisting);
    ASSERT_TRUE(journalFile2.IsOk());
    auto journalResult2 = PowerResilience::CreateWriteAheadJournal(std::move(journalFile2.Value()));
    std::shared_ptr<PowerResilience::IWriteAheadJournal> journal2 = std::move(journalResult2.Value());

    auto backingResult2 = Storage::OpenFileBackingStore(PathFor("race2.store"));
    std::shared_ptr<Storage::IBackingStore> backingStore2 = std::move(backingResult2.Value());

    auto engineResult2 = CoreEngine::CreateCacheEngine(
        CoreEngine::CacheEngineOptions{}, backingStore2, journal2);
    auto engine2 = std::move(engineResult2.Value());

    ASSERT_TRUE(engine2->ReplayFromJournal().IsOk());
    ASSERT_TRUE(engine2->MarkRecoveryComplete().IsOk());

    auto getResult = engine2->Get("racer2");
    ASSERT_TRUE(getResult.IsOk())
        << "the racer2 key must be recoverable after a simulated crash following the shutdown race";
    EXPECT_EQ(std::string(getResult.Value().begin(), getResult.Value().end()), "also-must-not-be-lost");
}
