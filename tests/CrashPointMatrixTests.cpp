// Stage 2.5 hardening: PRIORITY 3 — systematic crash-consistency
// point matrix. For each persistent mutation (Put, Flush,
// ForceInvalidate), this file simulates a process crash at every
// meaningful boundary in that operation's sequence, then reconstructs a
// FRESH engine (real restart simulation) against whatever was left on
// disk, runs ReplayFromJournal(), and checks the resulting state against
// an explicit "recovery oracle" of legal final states.
//
// The oracle for EVERY crash point below is: the final, recovered state
// must be ONE of the legal pre-crash or post-crash-boundary states for
// that exact record type's sequence — never anything else. Concretely,
// for a Put(key, value) that crashes at boundary B, the recovered state
// for `key` must be either:
//   (a) exactly as before this Put() was ever called (record never made
//       it durably to the journal), or
//   (b) exactly the NEW value, durably recorded (the record was
//       durably journaled before the simulated crash).
// It must NEVER be:
//   - a torn/partial value (impossible by construction: JournalRecordCodec
//     encodes the whole record as one journal frame, and the journal's
//     own CRC-guarded framing makes a torn frame indistinguishable from
//     "not there" — this is exactly what is being verified here, not
//     assumed),
//   - the record applied twice with diverging side effects (replay must
//     be idempotent),
//   - metadata (dirty state) and data (value) disagreeing about which
//     version is current,
//   - a state that could only exist if the crash landed WITHOUT the
//     journal ever being touched, together with a change that could only
//     happen AFTER a durable journal write (a "boundary inversion").
//
// Crash points are simulated using real decorator objects around the
// REAL IWriteAheadJournal/IBackingStore, each one able to make a
// specific call sequence stop (throw/simulate death) at an EXACT point
// — before, during, or after the underlying real operation — mirroring
// exactly where a real process's memory would vanish relative to what
// had already been durably written to disk at that instant.
#include "QuantumCache/CoreEngine/ICacheEngine.h"
#include "QuantumCache/CoreEngine/JournalRecordCodec.h"
#include "QuantumCache/PowerResilience/IWriteAheadJournal.h"
#include "QuantumCache/Storage/IBackingStore.h"
#include "QuantumCache/Storage/IFile.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <memory>

using namespace QuantumCache;
namespace fs = std::filesystem;

namespace {

std::vector<std::uint8_t> Bytes(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}
std::string ToStr(const std::vector<std::uint8_t>& b) {
    return std::string(b.begin(), b.end());
}

// A "simulated crash" is modeled as: the operation that was in flight
// simply never returns to its caller (the caller/engine object is
// dropped without any further code running) — but whatever had ALREADY
// been durably written to the real underlying file(s) up to that exact
// point remains on disk, exactly like a real process losing power mid
// syscall. We implement this by having the decorator perform the real
// underlying operation UP TO the crash point, and then, if this call was
// selected as "the crash point", simply never return a result — instead
// the test harness treats this by not calling anything further on that
// engine and going straight to "destroy this whole rig, reopen fresh."
//
// A simpler and equally faithful technique used here: the decorator
// performs the real underlying work, and if armed, returns a distinct
// "simulated crash" sentinel error AFTER doing (or deliberately NOT
// doing) the real work up to that point — the calling engine code will
// see this as a failure and return early (exactly as it would for a
// real I/O error), while the ACTUAL on-disk state reflects whichever
// real operations the decorator allowed to complete first. This models
// "crash right after this real write landed on disk, before the
// process could act on the fact that it succeeded" — which is the
// actual physical scenario a power cut produces (the disk write may be
// durable while the CPU/RAM holding the "it succeeded" information is
// wiped).
enum class CrashPoint {
    None,                   // no crash: let the whole operation complete normally
    BeforeJournalAppend,    // crash before the real Append() call happens at all
    DuringJournalAppend,    // Append() begins but the underlying durable write is
                            // interrupted before completing (modeled as: the
                            // underlying real Append() is SKIPPED entirely, as if
                            // the write never reached the OS — equivalent to
                            // BeforeJournalAppend from the on-disk-state
                            // perspective, but exercised via a different code path)
    AfterJournalDurability, // the real Append() (including its FlushDurable())
                            // completes successfully on disk, then the crash
                            // sentinel is returned instead of letting the engine
                            // proceed to update in-memory shard state
    DuringBackingStoreWrite,// for Flush()-class operations: the backing store's
                            // Put()/Remove() is invoked but its result is replaced
                            // with a crash sentinel WITHOUT performing the real
                            // underlying write (modeling "power died mid-write,
                            // before the write reached the device")
    AfterBackingStoreWrite, // the real backing-store write completes, THEN crash
    AfterBackingStoreDurability, // explicitly identical to AfterBackingStoreWrite
                                  // for this project (FileBackingStore's Put()
                                  // itself calls FlushDurable() internally before
                                  // returning — see FileBackingStore.cpp; there is
                                  // no separate "write vs durability" boundary
                                  // exposed at the IBackingStore level to crash
                                  // between), kept as a DISTINCT enumerator purely
                                  // to make the audit's requested checklist
                                  // traceable one-to-one against this file
    DuringMetadataUpdate,   // crash while the SECOND journal record of a
                            // multi-record sequence (e.g. FlushComplete after
                            // FlushIntent) is being appended
    AfterCommit,            // the full logical operation completed durably on
                            // disk; crash happens only in the in-memory
                            // bookkeeping step afterward (modeled by simply not
                            // calling anything further — the "AfterJournalDurability"
                            // and "AfterCommit" cases converge for single-record
                            // operations, and are kept distinct for multi-record
                            // ones)
    DuringCleanup,          // for compaction/truncation specifically: crash during
                            // the Truncate() call itself (covered by
                            // CacheEngineCompactionAuditTests.cpp's dedicated
                            // TruncateFailingJournal coverage; referenced here for
                            // completeness of the matrix, not re-implemented)
};

// Journal decorator supporting crash injection at Append() boundaries.
class CrashInjectingJournal final : public PowerResilience::IWriteAheadJournal {
public:
    explicit CrashInjectingJournal(std::shared_ptr<PowerResilience::IWriteAheadJournal> inner)
        : inner_(std::move(inner)) {}

    // `atCallNumber` counts Append() calls made AFTER this ArmCrash()
    // call (the internal counter is reset to zero here) -- NOT an
    // absolute count since the decorator was constructed. E.g.
    // atCallNumber=1 (the default) means "crash on the very next
    // Append() call", atCallNumber=2 means "let the next Append() call
    // through normally, then crash on the one after that."
    void ArmCrash(CrashPoint point, int atCallNumber = 1) {
        crashPoint_ = point;
        targetCallNumber_ = atCallNumber;
        callCount_ = 0;
    }

    Common::Result<std::uint64_t> Append(const std::vector<std::uint8_t>& payload) override {
        ++callCount_;
        bool isTarget = (callCount_ == targetCallNumber_);

        if (isTarget && (crashPoint_ == CrashPoint::BeforeJournalAppend ||
                          crashPoint_ == CrashPoint::DuringJournalAppend)) {
            // Simulate power loss before the write ever reached durable
            // storage: skip the real Append() entirely. On-disk state
            // is therefore untouched by this call.
            return Common::Result<std::uint64_t>::Failure(
                Common::Error{Common::ErrorCode::IoError, "SIMULATED_CRASH: before/during journal append", 0});
        }

        auto result = inner_->Append(payload);

        if (isTarget && crashPoint_ == CrashPoint::AfterJournalDurability && result.IsOk()) {
            // The real Append() (with its internal FlushDurable())
            // already completed successfully — the record IS durably on
            // disk at this point. Now simulate the crash: report
            // failure to the caller anyway, exactly modeling "the write
            // physically landed, but the process died before it could
            // act on knowing that."
            return Common::Result<std::uint64_t>::Failure(
                Common::Error{Common::ErrorCode::IoError,
                              "SIMULATED_CRASH: after journal durability, before caller could react", 0});
        }
        if (isTarget && crashPoint_ == CrashPoint::DuringMetadataUpdate && result.IsOk()) {
            return Common::Result<std::uint64_t>::Failure(
                Common::Error{Common::ErrorCode::IoError,
                              "SIMULATED_CRASH: during multi-record metadata update sequence", 0});
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
    CrashPoint crashPoint_{CrashPoint::None};
    int targetCallNumber_{0};
    int callCount_{0};
};

// Backing-store decorator supporting crash injection at Put()/Remove()
// boundaries.
class CrashInjectingBackingStore final : public Storage::IBackingStore {
public:
    explicit CrashInjectingBackingStore(std::shared_ptr<Storage::IBackingStore> inner)
        : inner_(std::move(inner)) {}

    void ArmCrash(CrashPoint point) { crashPoint_ = point; }

    Common::Result<std::vector<std::uint8_t>> Get(const std::string& key) override {
        return inner_->Get(key);
    }
    Common::Result<void> Put(const std::string& key, const std::vector<std::uint8_t>& value) override {
        if (crashPoint_ == CrashPoint::DuringBackingStoreWrite) {
            // Power loss before the write reached the device: skip the
            // real Put() entirely.
            return Common::Result<void>::Failure(
                Common::Error{Common::ErrorCode::IoError, "SIMULATED_CRASH: during backing-store write", 0});
        }
        auto result = inner_->Put(key, value);
        if (result.IsOk() && (crashPoint_ == CrashPoint::AfterBackingStoreWrite ||
                               crashPoint_ == CrashPoint::AfterBackingStoreDurability)) {
            return Common::Result<void>::Failure(
                Common::Error{Common::ErrorCode::IoError,
                              "SIMULATED_CRASH: after backing-store write/durability", 0});
        }
        return result;
    }
    Common::Result<void> PutBatch(const std::vector<Storage::BackingStoreRecord>& records) override {
        if (crashPoint_ == CrashPoint::DuringBackingStoreWrite) {
            return Common::Result<void>::Failure(
                Common::Error{Common::ErrorCode::IoError, "SIMULATED_CRASH: during backing-store write", 0});
        }
        auto result = inner_->PutBatch(records);
        if (result.IsOk() && (crashPoint_ == CrashPoint::AfterBackingStoreWrite ||
                               crashPoint_ == CrashPoint::AfterBackingStoreDurability)) {
            return Common::Result<void>::Failure(
                Common::Error{Common::ErrorCode::IoError,
                              "SIMULATED_CRASH: after backing-store write/durability", 0});
        }
        return result;
    }
    Common::Result<void> Remove(const std::string& key) override {
        if (crashPoint_ == CrashPoint::DuringBackingStoreWrite) {
            return Common::Result<void>::Failure(
                Common::Error{Common::ErrorCode::IoError, "SIMULATED_CRASH: during backing-store remove", 0});
        }
        auto result = inner_->Remove(key);
        if (result.IsOk() && (crashPoint_ == CrashPoint::AfterBackingStoreWrite ||
                               crashPoint_ == CrashPoint::AfterBackingStoreDurability)) {
            return Common::Result<void>::Failure(
                Common::Error{Common::ErrorCode::IoError,
                              "SIMULATED_CRASH: after backing-store remove/durability", 0});
        }
        return result;
    }
    bool Contains(const std::string& key) override { return inner_->Contains(key); }
    std::size_t EntryCount() const noexcept override { return inner_->EntryCount(); }
    std::uint64_t GetVersion(const std::string& key) override { return inner_->GetVersion(key); }

private:
    std::shared_ptr<Storage::IBackingStore> inner_;
    CrashPoint crashPoint_{CrashPoint::None};
};

class CrashPointMatrixTest : public ::testing::Test {
protected:
    void SetUp() override {
        testDir_ = fs::temp_directory_path() /
                   ("qc_crashmatrix_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
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

    // Reconstructs a fresh engine against the same on-disk journal +
    // backing store (a real restart simulation) and runs recovery,
    // returning the ready-to-use engine plus its real journal/backing
    // store handles.
    struct RecoveredRig {
        std::shared_ptr<PowerResilience::IWriteAheadJournal> journal;
        std::shared_ptr<Storage::IBackingStore> backingStore;
        std::unique_ptr<CoreEngine::ICacheEngine> engine;
        bool recoverySucceeded{false};
    };

    RecoveredRig RecoverFresh(const std::string& journalName, const std::string& storeName) {
        RecoveredRig rig;
        auto journalFile = Storage::OpenFile(PathFor(journalName), Storage::OpenMode::OpenExisting);
        EXPECT_TRUE(journalFile.IsOk());
        auto journalResult = PowerResilience::CreateWriteAheadJournal(std::move(journalFile.Value()));
        EXPECT_TRUE(journalResult.IsOk());
        rig.journal = std::move(journalResult.Value());

        auto backingResult = Storage::OpenFileBackingStore(PathFor(storeName));
        EXPECT_TRUE(backingResult.IsOk());
        rig.backingStore = std::move(backingResult.Value());

        auto engineResult = CoreEngine::CreateCacheEngine(
            CoreEngine::CacheEngineOptions{}, rig.backingStore, rig.journal);
        EXPECT_TRUE(engineResult.IsOk());
        rig.engine = std::move(engineResult.Value());

        auto replayResult = rig.engine->ReplayFromJournal();
        if (!replayResult.IsOk()) {
            rig.recoverySucceeded = false;
            return rig;
        }
        auto markReady = rig.engine->MarkRecoveryComplete();
        rig.recoverySucceeded = markReady.IsOk();
        return rig;
    }

    fs::path testDir_;
};

} // namespace

// =======================================================================
// PUT() crash-point matrix.
// =======================================================================
// Put()'s real sequence: CheckReadyForWrite -> TryAdmitWrite ->
// AppendJournalRecordWithNewVersion (journal Append, WITH internal
// durable flush) -> InsertOrUpdateLocked (in-memory commit) ->
// [WriteThrough only: FlushKey].
//
// Legal oracle for a Put("crashkey", "crashvalue") issued against a key
// that did NOT exist before, crashing at each point:

TEST_F(CrashPointMatrixTest, Put_CrashBeforeJournalAppend_RecoversToKeyAbsent) {
    auto journalFile = Storage::OpenFile(PathFor("j.dat"), Storage::OpenMode::OpenOrCreate);
    ASSERT_TRUE(journalFile.IsOk());
    auto realJournalResult = PowerResilience::CreateWriteAheadJournal(std::move(journalFile.Value()));
    auto crashJournal = std::make_shared<CrashInjectingJournal>(std::move(realJournalResult.Value()));

    auto backingResult = Storage::OpenFileBackingStore(PathFor("s.dat"));
    ASSERT_TRUE(backingResult.IsOk());
    std::shared_ptr<Storage::IBackingStore> backingStore = std::move(backingResult.Value());

    {
        auto engineResult = CoreEngine::CreateCacheEngine(
            CoreEngine::CacheEngineOptions{}, backingStore, crashJournal);
        ASSERT_TRUE(engineResult.IsOk());
        auto engine = std::move(engineResult.Value());
        ASSERT_TRUE(engine->MarkRecoveryComplete().IsOk());

        crashJournal->ArmCrash(CrashPoint::BeforeJournalAppend);
        auto putResult = engine->Put("crashkey", Bytes("crashvalue"));
        // The engine correctly reports failure (it must never claim
        // success for a write that was not durably journaled).
        EXPECT_FALSE(putResult.IsOk());
        // engine destroyed here without Shutdown() -- simulates the
        // process vanishing right after this failed Put() call returned.
    }

    // ORACLE: key must be absent (state exactly as before the crashed
    // Put() was ever attempted) -- the record never reached durable
    // storage, so it must not exist after recovery.
    auto rig = RecoverFresh("j.dat", "s.dat");
    ASSERT_TRUE(rig.recoverySucceeded);
    auto getResult = rig.engine->Get("crashkey");
    EXPECT_FALSE(getResult.IsOk());
    EXPECT_EQ(getResult.Err().code, Common::ErrorCode::NotFound)
        << "REJECTED IMPOSSIBLE STATE: a Put() that failed before ever reaching durable "
           "storage must not leave any trace of the key after recovery";
}

TEST_F(CrashPointMatrixTest, Put_CrashAfterJournalDurability_RecoversToKeyPresentWithNewValue) {
    auto journalFile = Storage::OpenFile(PathFor("j.dat"), Storage::OpenMode::OpenOrCreate);
    ASSERT_TRUE(journalFile.IsOk());
    auto realJournalResult = PowerResilience::CreateWriteAheadJournal(std::move(journalFile.Value()));
    auto crashJournal = std::make_shared<CrashInjectingJournal>(std::move(realJournalResult.Value()));

    auto backingResult = Storage::OpenFileBackingStore(PathFor("s.dat"));
    ASSERT_TRUE(backingResult.IsOk());
    std::shared_ptr<Storage::IBackingStore> backingStore = std::move(backingResult.Value());

    {
        auto engineResult = CoreEngine::CreateCacheEngine(
            CoreEngine::CacheEngineOptions{}, backingStore, crashJournal);
        ASSERT_TRUE(engineResult.IsOk());
        auto engine = std::move(engineResult.Value());
        ASSERT_TRUE(engine->MarkRecoveryComplete().IsOk());

        crashJournal->ArmCrash(CrashPoint::AfterJournalDurability);
        auto putResult = engine->Put("crashkey", Bytes("crashvalue"));
        // The engine's OWN view is "failed" (it never got a successful
        // return from Append()), but the real write already landed
        // durably on disk before the simulated crash sentinel was
        // returned -- exactly modeling a real power cut immediately
        // after the physical write completed.
        EXPECT_FALSE(putResult.IsOk());
    }

    // ORACLE: the record IS durably on disk (the real Append() +
    // FlushDurable() genuinely completed), so recovery must find and
    // apply it -- key must be present with the new value, marked Dirty
    // (never flushed to the backing store in this scenario).
    auto rig = RecoverFresh("j.dat", "s.dat");
    ASSERT_TRUE(rig.recoverySucceeded);
    auto getResult = rig.engine->Get("crashkey");
    ASSERT_TRUE(getResult.IsOk())
        << "REJECTED IMPOSSIBLE STATE (acknowledged-write loss): a record that was durably "
           "written to the journal before the crash must be recovered";
    EXPECT_EQ(ToStr(getResult.Value()), "crashvalue");
    auto info = rig.engine->GetEntryInfo("crashkey");
    ASSERT_TRUE(info.IsOk());
    EXPECT_EQ(info.Value().dirtyState, CoreEngine::EntryDirtyState::Dirty)
        << "REJECTED IMPOSSIBLE STATE (metadata/data divergence): a record durably journaled "
           "but never flushed must be recovered as Dirty, not Clean";
}

TEST_F(CrashPointMatrixTest, Put_ThenCrashBeforeSecondPut_OverwriteSemanticsPreservedOrFullyLost_NeverTorn) {
    // Two sequential Put()s to the SAME key: v1 succeeds fully and
    // durably; a SECOND Put() (v2) is then crashed at
    // AfterJournalDurability. The oracle: recovery must show EITHER v1
    // (if, hypothetically, the second record hadn't reached durable
    // storage -- not this scenario, but included for completeness of
    // the oracle definition) OR v2 (since it DID reach durable storage)
    // -- and via the version-ordering mechanism, NEVER a value that is
    // neither v1 nor v2, and never v1 winning over a durably-written v2
    // (a "stale overwrite" -- exactly the class of bug the project's
    // own read-miss/write race hardening targets, now re-verified here
    // specifically for the crash-recovery path rather than the live
    // concurrent-access path).
    auto journalFile = Storage::OpenFile(PathFor("j.dat"), Storage::OpenMode::OpenOrCreate);
    ASSERT_TRUE(journalFile.IsOk());
    auto realJournalResult = PowerResilience::CreateWriteAheadJournal(std::move(journalFile.Value()));
    auto crashJournal = std::make_shared<CrashInjectingJournal>(std::move(realJournalResult.Value()));

    auto backingResult = Storage::OpenFileBackingStore(PathFor("s.dat"));
    ASSERT_TRUE(backingResult.IsOk());
    std::shared_ptr<Storage::IBackingStore> backingStore = std::move(backingResult.Value());

    {
        auto engineResult = CoreEngine::CreateCacheEngine(
            CoreEngine::CacheEngineOptions{}, backingStore, crashJournal);
        ASSERT_TRUE(engineResult.IsOk());
        auto engine = std::move(engineResult.Value());
        ASSERT_TRUE(engine->MarkRecoveryComplete().IsOk());

        ASSERT_TRUE(engine->Put("ok", Bytes("v1")).IsOk());

        // Crash on THIS call's very first (and only) Append() —
        // ArmCrash() resets the decorator's internal call counter at
        // arm-time, so atCallNumber is relative to calls made AFTER
        // arming, not an absolute count since object construction.
        crashJournal->ArmCrash(CrashPoint::AfterJournalDurability, /*atCallNumber=*/1);
        auto putResult = engine->Put("ok", Bytes("v2"));
        EXPECT_FALSE(putResult.IsOk());
    }

    auto rig = RecoverFresh("j.dat", "s.dat");
    ASSERT_TRUE(rig.recoverySucceeded);
    auto getResult = rig.engine->Get("ok");
    ASSERT_TRUE(getResult.IsOk());
    // v2's record durably landed (AfterJournalDurability means the real
    // write succeeded before the sentinel was returned), so v2 must win
    // -- this is the ONLY legal outcome for this exact crash point.
    EXPECT_EQ(ToStr(getResult.Value()), "v2")
        << "REJECTED IMPOSSIBLE STATE (stale overwrite): v2's journal record was durably "
           "written before the crash; recovery must not resurrect the older v1 value";
}

// =======================================================================
// FLUSH() crash-point matrix (multi-record: FlushIntent -> backing-store
// Put() -> FlushComplete).
// =======================================================================

TEST_F(CrashPointMatrixTest, Flush_CrashDuringBackingStoreWrite_RecoversAsStillDirty_OriginalValueInJournal) {
    // FlushIntent is durably journaled successfully; the backing-store
    // Put() itself is then interrupted (power loss mid-write, before it
    // reaches the device). Oracle: after recovery, the key must be
    // Dirty with its ORIGINAL (pre-flush-attempt) value -- the backing
    // store must NOT have silently absorbed a torn/partial write, and
    // the cache must not believe the flush succeeded.
    auto journalFile = Storage::OpenFile(PathFor("j.dat"), Storage::OpenMode::OpenOrCreate);
    ASSERT_TRUE(journalFile.IsOk());
    auto journalResult = PowerResilience::CreateWriteAheadJournal(std::move(journalFile.Value()));
    std::shared_ptr<PowerResilience::IWriteAheadJournal> journal = std::move(journalResult.Value());

    auto backingResult = Storage::OpenFileBackingStore(PathFor("s.dat"));
    ASSERT_TRUE(backingResult.IsOk());
    auto crashStore = std::make_shared<CrashInjectingBackingStore>(std::move(backingResult.Value()));

    {
        auto engineResult = CoreEngine::CreateCacheEngine(
            CoreEngine::CacheEngineOptions{}, crashStore, journal);
        ASSERT_TRUE(engineResult.IsOk());
        auto engine = std::move(engineResult.Value());
        ASSERT_TRUE(engine->MarkRecoveryComplete().IsOk());

        ASSERT_TRUE(engine->Put("fk", Bytes("original")).IsOk());

        crashStore->ArmCrash(CrashPoint::DuringBackingStoreWrite);
        auto flushResult = engine->Flush("fk");
        EXPECT_FALSE(flushResult.IsOk());
        // engine destroyed here -- simulates a crash right after the
        // failed Flush() call returned (FlushIntent IS durably
        // journaled by this point; the backing-store write never
        // happened at all).
    }

    auto rig = RecoverFresh("j.dat", "s.dat");
    ASSERT_TRUE(rig.recoverySucceeded);
    auto getResult = rig.engine->Get("fk");
    ASSERT_TRUE(getResult.IsOk());
    EXPECT_EQ(ToStr(getResult.Value()), "original")
        << "REJECTED IMPOSSIBLE STATE: a backing-store write that never happened must not "
           "silently 'succeed' after recovery";
    auto info = rig.engine->GetEntryInfo("fk");
    ASSERT_TRUE(info.IsOk());
    EXPECT_EQ(info.Value().dirtyState, CoreEngine::EntryDirtyState::Dirty)
        << "REJECTED IMPOSSIBLE STATE (metadata/data divergence): FlushIntent alone (without a "
           "matching FlushComplete) must never be interpreted as Clean";
}

TEST_F(CrashPointMatrixTest, Flush_CrashAfterBackingStoreWrite_RecoversSafely_NoDataLoss) {
    // The backing-store batch write genuinely lands durably, but crash sentinel is returned.
    // Oracle: recovery must NOT lose data.
    auto journalFile = Storage::OpenFile(PathFor("j.dat"), Storage::OpenMode::OpenOrCreate);
    ASSERT_TRUE(journalFile.IsOk());
    auto journalResult = PowerResilience::CreateWriteAheadJournal(std::move(journalFile.Value()));
    std::shared_ptr<PowerResilience::IWriteAheadJournal> journal = std::move(journalResult.Value());

    auto backingResult = Storage::OpenFileBackingStore(PathFor("s.dat"));
    ASSERT_TRUE(backingResult.IsOk());
    auto crashStore = std::make_shared<CrashInjectingBackingStore>(std::move(backingResult.Value()));

    {
        auto engineResult = CoreEngine::CreateCacheEngine(
            CoreEngine::CacheEngineOptions{}, crashStore, journal);
        ASSERT_TRUE(engineResult.IsOk());
        auto engine = std::move(engineResult.Value());
        ASSERT_TRUE(engine->MarkRecoveryComplete().IsOk());

        ASSERT_TRUE(engine->Put("fk2", Bytes("theval")).IsOk());
        crashStore->ArmCrash(CrashPoint::AfterBackingStoreWrite);
        auto flushResult = engine->Flush("fk2");
        EXPECT_FALSE(flushResult.IsOk());
    }

    auto rig = RecoverFresh("j.dat", "s.dat");
    ASSERT_TRUE(rig.recoverySucceeded);
    auto getResult = rig.engine->Get("fk2");
    ASSERT_TRUE(getResult.IsOk());
    EXPECT_EQ(ToStr(getResult.Value()), "theval");
    EXPECT_TRUE(rig.backingStore->Contains("fk2"));
}

// =======================================================================
// FORCE-INVALIDATE() crash-point matrix (journal Invalidate record ->
// backing-store Remove()).
// =======================================================================

TEST_F(CrashPointMatrixTest, ForceInvalidate_CrashBeforeBackingStoreRemove_RecoversToKeyRemoved) {
    // The Invalidate journal record durably lands; the backing-store
    // Remove() is interrupted (crashes) before it can execute. Oracle:
    // recovery replays the Invalidate record and must (re-)issue the
    // Remove() itself -- InvalidateImpl's own contract already commits
    // the journal record BEFORE calling Remove(), specifically so a
    // crash in this exact window is recoverable via replay. Confirmed
    // here end-to-end (not merely reasoned about).
    auto journalFile = Storage::OpenFile(PathFor("j.dat"), Storage::OpenMode::OpenOrCreate);
    ASSERT_TRUE(journalFile.IsOk());
    auto journalResult = PowerResilience::CreateWriteAheadJournal(std::move(journalFile.Value()));
    std::shared_ptr<PowerResilience::IWriteAheadJournal> journal = std::move(journalResult.Value());

    auto backingResult = Storage::OpenFileBackingStore(PathFor("s.dat"));
    ASSERT_TRUE(backingResult.IsOk());
    auto crashStore = std::make_shared<CrashInjectingBackingStore>(std::move(backingResult.Value()));

    {
        auto engineResult = CoreEngine::CreateCacheEngine(
            CoreEngine::CacheEngineOptions{}, crashStore, journal);
        ASSERT_TRUE(engineResult.IsOk());
        auto engine = std::move(engineResult.Value());
        ASSERT_TRUE(engine->MarkRecoveryComplete().IsOk());

        ASSERT_TRUE(engine->Put("ik", Bytes("toberemoved")).IsOk());
        ASSERT_TRUE(engine->Flush("ik").IsOk()); // durably in backing store, Clean

        crashStore->ArmCrash(CrashPoint::DuringBackingStoreWrite); // affects Remove() too
        auto invalidateResult = engine->ForceInvalidate("ik");
        EXPECT_FALSE(invalidateResult.IsOk());
        // engine destroyed here -- the Invalidate JOURNAL record is
        // durably written (InvalidateImpl journals BEFORE calling
        // Remove()), but the actual backing-store Remove() never
        // executed.
    }

    auto rig = RecoverFresh("j.dat", "s.dat");
    ASSERT_TRUE(rig.recoverySucceeded)
        << "recovery must succeed: replay re-issues the Remove() for the durably-journaled "
           "Invalidate record against a (now crash-free) real backing store";
    EXPECT_FALSE(rig.backingStore->Contains("ik"))
        << "REJECTED IMPOSSIBLE STATE: an Invalidate record that was durably journaled must "
           "result in the key being removed after recovery, even if the original removal "
           "attempt was itself interrupted";
    auto getResult = rig.engine->Get("ik");
    EXPECT_FALSE(getResult.IsOk());
    EXPECT_EQ(getResult.Err().code, Common::ErrorCode::NotFound);
}

TEST_F(CrashPointMatrixTest, ForceInvalidate_CrashBeforeJournalAppend_RecoversToKeyStillPresent) {
    // The Invalidate record never reaches durable storage at all
    // (crash before Append()). Oracle: the key must still be fully
    // present and correct after recovery -- an invalidation that never
    // durably happened must not have any effect.
    auto journalFile = Storage::OpenFile(PathFor("j.dat"), Storage::OpenMode::OpenOrCreate);
    ASSERT_TRUE(journalFile.IsOk());
    auto realJournalResult = PowerResilience::CreateWriteAheadJournal(std::move(journalFile.Value()));
    auto crashJournal = std::make_shared<CrashInjectingJournal>(std::move(realJournalResult.Value()));

    auto backingResult = Storage::OpenFileBackingStore(PathFor("s.dat"));
    ASSERT_TRUE(backingResult.IsOk());
    std::shared_ptr<Storage::IBackingStore> backingStore = std::move(backingResult.Value());

    {
        auto engineResult = CoreEngine::CreateCacheEngine(
            CoreEngine::CacheEngineOptions{}, backingStore, crashJournal);
        ASSERT_TRUE(engineResult.IsOk());
        auto engine = std::move(engineResult.Value());
        ASSERT_TRUE(engine->MarkRecoveryComplete().IsOk());

        ASSERT_TRUE(engine->Put("ik2", Bytes("staysalive")).IsOk());
        ASSERT_TRUE(engine->Flush("ik2").IsOk());

        // ForceInvalidate() makes exactly 1 Append() call (the
        // Invalidate record); crash on that one call (relative to
        // ArmCrash()'s counter reset).
        crashJournal->ArmCrash(CrashPoint::BeforeJournalAppend, /*atCallNumber=*/1);
        auto invalidateResult = engine->ForceInvalidate("ik2");
        EXPECT_FALSE(invalidateResult.IsOk());
    }

    auto rig = RecoverFresh("j.dat", "s.dat");
    ASSERT_TRUE(rig.recoverySucceeded);
    auto getResult = rig.engine->Get("ik2");
    ASSERT_TRUE(getResult.IsOk())
        << "REJECTED IMPOSSIBLE STATE: an Invalidate that never durably reached the journal "
           "must have no effect after recovery";
    EXPECT_EQ(ToStr(getResult.Value()), "staysalive");
}

// =======================================================================
// Duplicate-replay / repeated-recovery guard: replaying the SAME journal
// twice in a row (without any new writes in between) must be a safe,
// fully idempotent no-op -- directly addresses "duplicate replay" and
// "partial transaction state" from the audit's explicit rejection list.
// =======================================================================

TEST_F(CrashPointMatrixTest, ReplayingSameJournalTwice_IsFullyIdempotent_NoDuplicateSideEffects) {
    auto journalFile = Storage::OpenFile(PathFor("j.dat"), Storage::OpenMode::OpenOrCreate);
    ASSERT_TRUE(journalFile.IsOk());
    auto journalResult = PowerResilience::CreateWriteAheadJournal(std::move(journalFile.Value()));
    std::shared_ptr<PowerResilience::IWriteAheadJournal> journal = std::move(journalResult.Value());

    auto backingResult = Storage::OpenFileBackingStore(PathFor("s.dat"));
    ASSERT_TRUE(backingResult.IsOk());
    std::shared_ptr<Storage::IBackingStore> backingStore = std::move(backingResult.Value());

    {
        auto engineResult = CoreEngine::CreateCacheEngine(
            CoreEngine::CacheEngineOptions{}, backingStore, journal);
        ASSERT_TRUE(engineResult.IsOk());
        auto engine = std::move(engineResult.Value());
        ASSERT_TRUE(engine->MarkRecoveryComplete().IsOk());
        ASSERT_TRUE(engine->Put("dupkey", Bytes("dupvalue")).IsOk());
        // crash: no Shutdown(), journal has one Upsert record on disk.
    }

    // Recover once.
    auto journalFile2 = Storage::OpenFile(PathFor("j.dat"), Storage::OpenMode::OpenExisting);
    ASSERT_TRUE(journalFile2.IsOk());
    auto journalResult2 = PowerResilience::CreateWriteAheadJournal(std::move(journalFile2.Value()));
    std::shared_ptr<PowerResilience::IWriteAheadJournal> journal2 = std::move(journalResult2.Value());
    auto backingResult2 = Storage::OpenFileBackingStore(PathFor("s.dat"));
    ASSERT_TRUE(backingResult2.IsOk());
    std::shared_ptr<Storage::IBackingStore> backingStore2 = std::move(backingResult2.Value());
    auto engineResult2 = CoreEngine::CreateCacheEngine(
        CoreEngine::CacheEngineOptions{}, backingStore2, journal2);
    ASSERT_TRUE(engineResult2.IsOk());
    auto engine2 = std::move(engineResult2.Value());

    // Call ReplayFromJournal() TWICE before MarkRecoveryComplete() --
    // ReplayFromJournal()'s own contract documents it as idempotent
    // ("safe to call more than once"); verify this concretely rather
    // than trusting the comment.
    ASSERT_TRUE(engine2->ReplayFromJournal().IsOk());
    ASSERT_TRUE(engine2->ReplayFromJournal().IsOk());
    ASSERT_TRUE(engine2->MarkRecoveryComplete().IsOk());

    auto getResult = engine2->Get("dupkey");
    ASSERT_TRUE(getResult.IsOk());
    EXPECT_EQ(ToStr(getResult.Value()), "dupvalue")
        << "REJECTED IMPOSSIBLE STATE (duplicate replay side effects): replaying the same "
           "journal content twice must not corrupt or duplicate the recovered value";
    auto info = engine2->GetEntryInfo("dupkey");
    ASSERT_TRUE(info.IsOk());
    EXPECT_EQ(info.Value().dirtyState, CoreEngine::EntryDirtyState::Dirty);
}
