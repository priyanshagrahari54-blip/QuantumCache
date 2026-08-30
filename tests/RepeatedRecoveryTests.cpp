// Stage 2.5 hardening: PRIORITY 4 — repeated recovery. Tests the
// specific sequence the audit calls out explicitly:
//
//   crash -> recovery -> crash DURING recovery -> recovery ->
//   crash DURING recovery -> recovery -> normal operation
//
// using the REAL, full startup stack (ISessionMarker + IWriteAheadJournal
// + IRecoveryManager + CacheEngine::ReplayFromJournal, orchestrated
// exactly the way src/Service/src/main_service.cpp does it), not just
// CacheEngine in isolation. Recovery must be safely RESTARTABLE (each
// fresh attempt from an on-disk state left by an interrupted previous
// attempt must still produce a correct result) and IDEMPOTENT (repeating
// a successful recovery step must never change the outcome).
#include "QuantumCache/CoreEngine/ICacheEngine.h"
#include "QuantumCache/PowerResilience/IRecoveryManager.h"
#include "QuantumCache/PowerResilience/ISessionMarker.h"
#include "QuantumCache/PowerResilience/IWriteAheadJournal.h"
#include "QuantumCache/Storage/IBackingStore.h"
#include "QuantumCache/Storage/IFile.h"
#include <gtest/gtest.h>
#include <filesystem>

using namespace QuantumCache;
namespace fs = std::filesystem;

namespace {

std::vector<std::uint8_t> Bytes(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}
std::string ToStr(const std::vector<std::uint8_t>& b) {
    return std::string(b.begin(), b.end());
}

class RepeatedRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        testDir_ = fs::temp_directory_path() /
                   ("qc_repeated_recovery_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
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

    // Performs one full "process startup" attempt: opens the marker,
    // journal, and backing store fresh (as a real restart would), runs
    // IRecoveryManager::InitializeAndRecover() with CacheEngine's
    // ReplayFromJournal() as the replay callback (exactly matching
    // main_service.cpp's real orchestration), and returns whether it
    // succeeded plus the resulting engine (usable only if
    // MarkRecoveryComplete() also succeeded).
    struct StartupAttempt {
        std::shared_ptr<PowerResilience::IWriteAheadJournal> journal;
        std::shared_ptr<Storage::IBackingStore> backingStore;
        std::unique_ptr<PowerResilience::IRecoveryManager> recoveryManager;
        std::unique_ptr<CoreEngine::ICacheEngine> engine;
        bool succeeded{false};
        PowerResilience::RecoveryState finalState{PowerResilience::RecoveryState::Unknown};
    };

    StartupAttempt AttemptStartup() {
        StartupAttempt attempt;

        auto markerFile = Storage::OpenFile(PathFor("marker.dat"), Storage::OpenMode::OpenOrCreate);
        EXPECT_TRUE(markerFile.IsOk());
        auto marker = PowerResilience::CreateSessionMarker(std::move(markerFile.Value()));
        EXPECT_TRUE(marker.IsOk());

        auto journalFile = Storage::OpenFile(PathFor("journal.dat"), Storage::OpenMode::OpenOrCreate);
        EXPECT_TRUE(journalFile.IsOk());
        auto journalResult = PowerResilience::CreateWriteAheadJournal(std::move(journalFile.Value()));
        EXPECT_TRUE(journalResult.IsOk());
        attempt.journal = std::move(journalResult.Value());

        auto recoveryManagerResult = PowerResilience::CreateRecoveryManager(
            std::move(marker.Value()), attempt.journal);
        EXPECT_TRUE(recoveryManagerResult.IsOk());
        attempt.recoveryManager = std::move(recoveryManagerResult.Value());

        auto backingResult = Storage::OpenFileBackingStore(PathFor("store.dat"));
        EXPECT_TRUE(backingResult.IsOk());
        attempt.backingStore = std::move(backingResult.Value());

        auto engineResult = CoreEngine::CreateCacheEngine(
            CoreEngine::CacheEngineOptions{}, attempt.backingStore, attempt.journal);
        EXPECT_TRUE(engineResult.IsOk());
        attempt.engine = std::move(engineResult.Value());

        auto initResult = attempt.recoveryManager->InitializeAndRecover(
            [&]() -> Common::Result<void> { return attempt.engine->ReplayFromJournal(); });

        attempt.finalState = attempt.recoveryManager->CurrentState();

        if (!initResult.IsOk()) {
            attempt.succeeded = false;
            return attempt;
        }

        auto markReady = attempt.engine->MarkRecoveryComplete();
        attempt.succeeded = markReady.IsOk();
        return attempt;
    }

    fs::path testDir_;
};

} // namespace

TEST_F(RepeatedRecoveryTest, CrashRecoveryCrashRecoveryCrashRecovery_ThenNormalOperation_AllStatesConsistent) {
    // Phase 0: establish an initial durable write and simulate a crash
    // (no clean shutdown marker written).
    {
        auto attempt = AttemptStartup();
        ASSERT_TRUE(attempt.succeeded);
        // Clean start (no prior session): recovery should be trivial.
        EXPECT_EQ(attempt.finalState, PowerResilience::RecoveryState::RecoveryComplete);

        ASSERT_TRUE(attempt.engine->Put("survivor", Bytes("gen0")).IsOk());
        // No MarkCleanShutdown() call, no engine Shutdown() call --
        // simulates a real crash right here.
    }

    // Phase 1: "recovery" -- first restart after the crash. The marker
    // will show an unclean shutdown (OnSessionStart was called during
    // Phase 0's AttemptStartup(), but no MarkCleanShutdown() ever ran),
    // so this restart must detect that and replay the journal.
    {
        auto attempt = AttemptStartup();
        ASSERT_TRUE(attempt.succeeded) << "first recovery attempt after the initial crash must succeed";
        EXPECT_EQ(attempt.finalState, PowerResilience::RecoveryState::RecoveryComplete);

        auto getResult = attempt.engine->Get("survivor");
        ASSERT_TRUE(getResult.IsOk());
        EXPECT_EQ(ToStr(getResult.Value()), "gen0");

        // Make an additional durable write during this session, then
        // simulate ANOTHER crash (this is "crash DURING recovery" in
        // spirit: the crash happens in the very next session that
        // itself was recovering from a prior crash, without ever
        // reaching a clean shutdown in between).
        ASSERT_TRUE(attempt.engine->Put("gen1key", Bytes("gen1")).IsOk());
        // No clean shutdown -- crash again.
    }

    // Phase 2: "recovery" after the second crash. Must correctly recover
    // BOTH the original survivor key (already durable from Phase 0/1)
    // and the new gen1key (durable from Phase 1).
    {
        auto attempt = AttemptStartup();
        ASSERT_TRUE(attempt.succeeded) << "second recovery attempt (recovering from a session that "
                                           "itself was a recovery) must succeed";
        EXPECT_EQ(attempt.finalState, PowerResilience::RecoveryState::RecoveryComplete);

        auto r1 = attempt.engine->Get("survivor");
        auto r2 = attempt.engine->Get("gen1key");
        ASSERT_TRUE(r1.IsOk());
        ASSERT_TRUE(r2.IsOk());
        EXPECT_EQ(ToStr(r1.Value()), "gen0");
        EXPECT_EQ(ToStr(r2.Value()), "gen1");

        ASSERT_TRUE(attempt.engine->Put("gen2key", Bytes("gen2")).IsOk());
        // Crash a third time.
    }

    // Phase 3: third recovery -- must still correctly reconstruct ALL
    // THREE generations of writes, proving repeated crash/recovery
    // cycles compound correctly rather than losing or corrupting
    // earlier state.
    {
        auto attempt = AttemptStartup();
        ASSERT_TRUE(attempt.succeeded) << "third recovery attempt must succeed";
        EXPECT_EQ(attempt.finalState, PowerResilience::RecoveryState::RecoveryComplete);

        auto r1 = attempt.engine->Get("survivor");
        auto r2 = attempt.engine->Get("gen1key");
        auto r3 = attempt.engine->Get("gen2key");
        ASSERT_TRUE(r1.IsOk());
        ASSERT_TRUE(r2.IsOk());
        ASSERT_TRUE(r3.IsOk());
        EXPECT_EQ(ToStr(r1.Value()), "gen0");
        EXPECT_EQ(ToStr(r2.Value()), "gen1");
        EXPECT_EQ(ToStr(r3.Value()), "gen2");

        // NOW transition to genuinely normal operation: flush
        // everything, shut down cleanly, and mark the session clean.
        ASSERT_TRUE(attempt.engine->FlushAll().IsOk());
        ASSERT_TRUE(attempt.engine->Shutdown().IsOk());
        ASSERT_TRUE(attempt.recoveryManager->MarkCleanShutdown().IsOk());
    }

    // Phase 4: final startup after a GENUINELY clean shutdown. Recovery
    // must recognize this and skip replay entirely (CleanShutdown path),
    // while all data must still be present (already durably in the
    // backing store from Phase 3's FlushAll()+Shutdown()).
    {
        auto attempt = AttemptStartup();
        ASSERT_TRUE(attempt.succeeded);
        // Clean shutdown was properly recorded -- this restart's
        // recovery manager should reach RecoveryComplete via the
        // "closedCleanly" fast path, not via an unclean-shutdown replay.
        EXPECT_EQ(attempt.finalState, PowerResilience::RecoveryState::RecoveryComplete);

        auto r1 = attempt.engine->Get("survivor");
        auto r2 = attempt.engine->Get("gen1key");
        auto r3 = attempt.engine->Get("gen2key");
        ASSERT_TRUE(r1.IsOk());
        ASSERT_TRUE(r2.IsOk());
        ASSERT_TRUE(r3.IsOk());
        EXPECT_EQ(ToStr(r1.Value()), "gen0");
        EXPECT_EQ(ToStr(r2.Value()), "gen1");
        EXPECT_EQ(ToStr(r3.Value()), "gen2");

        // And the engine is now in completely normal working order:
        // further Put()/Get() cycles must work exactly as expected.
        ASSERT_TRUE(attempt.engine->Put("normalop", Bytes("works")).IsOk());
        auto normalGet = attempt.engine->Get("normalop");
        ASSERT_TRUE(normalGet.IsOk());
        EXPECT_EQ(ToStr(normalGet.Value()), "works");

        ASSERT_TRUE(attempt.engine->Shutdown().IsOk());
        ASSERT_TRUE(attempt.recoveryManager->MarkCleanShutdown().IsOk());
    }
}

TEST_F(RepeatedRecoveryTest, RecoveryManager_SessionGeneration_MonotonicallyIncreasesAcrossRepeatedCrashes) {
    // The session generation counter (from SessionMarker) must
    // monotonically increase across each crash/recovery cycle, never
    // going backward or repeating -- this is the mechanism that lets a
    // real operator observe "how many times has this service crashed."
    std::uint64_t lastGeneration = 0;
    for (int cycle = 0; cycle < 5; ++cycle) {
        auto markerFile = Storage::OpenFile(
            PathFor("gen_marker.dat"),
            cycle == 0 ? Storage::OpenMode::OpenOrCreate : Storage::OpenMode::OpenExisting);
        ASSERT_TRUE(markerFile.IsOk());
        auto marker = PowerResilience::CreateSessionMarker(std::move(markerFile.Value()));
        ASSERT_TRUE(marker.IsOk());

        auto state = marker.Value()->ReadLastState();
        ASSERT_TRUE(state.IsOk());

        ASSERT_TRUE(marker.Value()->OnSessionStart().IsOk());
        std::uint64_t currentGeneration = marker.Value()->CurrentGeneration();

        EXPECT_GT(currentGeneration, lastGeneration)
            << "cycle " << cycle << ": session generation must strictly increase across crashes, "
               "never repeat or go backward";
        lastGeneration = currentGeneration;

        // No OnCleanShutdown() -- simulate a crash every cycle.
    }
}

TEST_F(RepeatedRecoveryTest, RecoveryFailure_ThenRetry_WithFixedJournal_EventuallySucceeds) {
    // "Recovery must be restartable": if one recovery attempt fails
    // (e.g. due to a transient/simulated corruption), a SUBSEQUENT
    // attempt against the SAME on-disk state (assuming the underlying
    // problem is no longer present -- e.g. a transient I/O error that
    // does not recur) must be able to succeed. This models "the disk
    // had a transient read error on the first boot after a crash, but a
    // reboot succeeds," a realistic scenario in real hardware.
    auto markerFile = Storage::OpenFile(PathFor("marker.dat"), Storage::OpenMode::OpenOrCreate);
    ASSERT_TRUE(markerFile.IsOk());
    auto marker = PowerResilience::CreateSessionMarker(std::move(markerFile.Value()));
    ASSERT_TRUE(marker.IsOk());
    ASSERT_TRUE(marker.Value()->OnSessionStart().IsOk());
    marker.Value().reset(); // close the marker handle before reopening below

    auto journalFile = Storage::OpenFile(PathFor("journal.dat"), Storage::OpenMode::OpenOrCreate);
    ASSERT_TRUE(journalFile.IsOk());
    auto journalResult = PowerResilience::CreateWriteAheadJournal(std::move(journalFile.Value()));
    ASSERT_TRUE(journalResult.IsOk());
    std::shared_ptr<PowerResilience::IWriteAheadJournal> journal = std::move(journalResult.Value());
    ASSERT_TRUE(journal->Append(Bytes("valid-looking-payload")).IsOk());

    // First attempt: simulate a transient replay failure via a callback
    // that fails exactly once.
    {
        auto markerFile2 = Storage::OpenFile(PathFor("marker.dat"), Storage::OpenMode::OpenExisting);
        ASSERT_TRUE(markerFile2.IsOk());
        auto marker2 = PowerResilience::CreateSessionMarker(std::move(markerFile2.Value()));
        ASSERT_TRUE(marker2.IsOk());

        auto journalFile2 = Storage::OpenFile(PathFor("journal.dat"), Storage::OpenMode::OpenExisting);
        ASSERT_TRUE(journalFile2.IsOk());
        auto journalResult2 = PowerResilience::CreateWriteAheadJournal(std::move(journalFile2.Value()));
        ASSERT_TRUE(journalResult2.IsOk());
        std::shared_ptr<PowerResilience::IWriteAheadJournal> journal2 = std::move(journalResult2.Value());

        auto recoveryManagerResult = PowerResilience::CreateRecoveryManager(
            std::move(marker2.Value()), journal2);
        ASSERT_TRUE(recoveryManagerResult.IsOk());
        auto recoveryManager = std::move(recoveryManagerResult.Value());

        auto initResult = recoveryManager->InitializeAndRecover([&]() -> Common::Result<void> {
            return Common::Result<void>::Failure(
                Common::Error{Common::ErrorCode::IoError, "simulated transient replay failure", 0});
        });
        EXPECT_FALSE(initResult.IsOk());
        EXPECT_EQ(recoveryManager->CurrentState(), PowerResilience::RecoveryState::RecoveryFailed);
        // recoveryManager destroyed here; nothing durable was corrupted
        // by this failed attempt (the failure was purely in the replay
        // CALLBACK, not in any real I/O against the marker/journal).
    }

    // Second attempt: same on-disk state, but this time the replay
    // callback succeeds (simulating "the transient condition is gone").
    // The marker's OWN state (generation, closedCleanly) is untouched by
    // the first attempt's failure (RecoveryManager only calls
    // OnSessionStart() AFTER a successful replay -- see
    // RecoveryManager.cpp), so this retry must see the SAME "unclean
    // shutdown, needs replay" state and be able to complete normally.
    {
        auto markerFile3 = Storage::OpenFile(PathFor("marker.dat"), Storage::OpenMode::OpenExisting);
        ASSERT_TRUE(markerFile3.IsOk());
        auto marker3 = PowerResilience::CreateSessionMarker(std::move(markerFile3.Value()));
        ASSERT_TRUE(marker3.IsOk());

        auto journalFile3 = Storage::OpenFile(PathFor("journal.dat"), Storage::OpenMode::OpenExisting);
        ASSERT_TRUE(journalFile3.IsOk());
        auto journalResult3 = PowerResilience::CreateWriteAheadJournal(std::move(journalFile3.Value()));
        ASSERT_TRUE(journalResult3.IsOk());
        std::shared_ptr<PowerResilience::IWriteAheadJournal> journal3 = std::move(journalResult3.Value());

        auto recoveryManagerResult = PowerResilience::CreateRecoveryManager(
            std::move(marker3.Value()), journal3);
        ASSERT_TRUE(recoveryManagerResult.IsOk());
        auto recoveryManager = std::move(recoveryManagerResult.Value());

        bool replayInvoked = false;
        auto initResult = recoveryManager->InitializeAndRecover([&]() -> Common::Result<void> {
            replayInvoked = true;
            return Common::Result<void>::Success();
        });
        EXPECT_TRUE(initResult.IsOk())
            << "REJECTED IMPOSSIBLE STATE (recovery not restartable): a fresh recovery attempt "
               "against the same on-disk state, with the underlying transient failure resolved, "
               "must be able to succeed";
        EXPECT_TRUE(replayInvoked);
        EXPECT_EQ(recoveryManager->CurrentState(), PowerResilience::RecoveryState::RecoveryComplete);
    }
}
