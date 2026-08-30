// These tests exercise the REAL crash-recovery logic (SessionMarker,
// WriteAheadJournal, RecoveryManager) against PortableFile — the
// Linux-testable reference IFile implementation. They are not a
// substitute for testing Win32File/real NTFS behavior on Windows, but
// they do genuinely exercise the state machine and journal replay
// algorithm, including simulated torn writes, which is the actual
// mechanism this project uses to detect/recover from the laptop's power
// cuts.
#include "QuantumCache/PowerResilience/ISessionMarker.h"
#include "QuantumCache/PowerResilience/IWriteAheadJournal.h"
#include "QuantumCache/PowerResilience/IRecoveryManager.h"
#include "QuantumCache/Storage/IFile.h"
#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>

using namespace QuantumCache;
namespace fs = std::filesystem;

namespace {

class PowerResilienceTest : public ::testing::Test {
protected:
    void SetUp() override {
        testDir_ = fs::temp_directory_path() / ("qc_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
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

TEST_F(PowerResilienceTest, FreshStore_NoMarker_ReportsCleanShutdown) {
    auto file = Storage::OpenFile(PathFor("session.marker"), Storage::OpenMode::OpenOrCreate);
    ASSERT_TRUE(file.IsOk());

    auto marker = PowerResilience::CreateSessionMarker(std::move(file.Value()));
    ASSERT_TRUE(marker.IsOk());

    auto state = marker.Value()->ReadLastState();
    ASSERT_TRUE(state.IsOk());
    EXPECT_TRUE(state.Value().closedCleanly);
    EXPECT_EQ(state.Value().generation, 0u);
}

TEST_F(PowerResilienceTest, SessionStart_ThenCleanShutdown_IsDetectedAsClean) {
    auto path = PathFor("session.marker");

    {
        auto file = Storage::OpenFile(path, Storage::OpenMode::OpenOrCreate);
        ASSERT_TRUE(file.IsOk());
        auto marker = PowerResilience::CreateSessionMarker(std::move(file.Value()));
        ASSERT_TRUE(marker.IsOk());
        ASSERT_TRUE(marker.Value()->OnSessionStart().IsOk());
        ASSERT_TRUE(marker.Value()->OnCleanShutdown().IsOk());
    }

    // Reopen fresh, as the next process launch would.
    auto file2 = Storage::OpenFile(path, Storage::OpenMode::OpenExisting);
    ASSERT_TRUE(file2.IsOk());
    auto marker2 = PowerResilience::CreateSessionMarker(std::move(file2.Value()));
    ASSERT_TRUE(marker2.IsOk());

    auto state = marker2.Value()->ReadLastState();
    ASSERT_TRUE(state.IsOk());
    EXPECT_TRUE(state.Value().closedCleanly);
    EXPECT_EQ(state.Value().generation, 1u);
}

TEST_F(PowerResilienceTest, SessionStart_WithoutCleanShutdown_IsDetectedAsUnclean) {
    // This is the actual scenario this laptop experiences: SessionStart()
    // is durably committed, then the process disappears (power cut)
    // before OnCleanShutdown() ever runs.
    auto path = PathFor("session.marker");

    {
        auto file = Storage::OpenFile(path, Storage::OpenMode::OpenOrCreate);
        ASSERT_TRUE(file.IsOk());
        auto marker = PowerResilience::CreateSessionMarker(std::move(file.Value()));
        ASSERT_TRUE(marker.IsOk());
        ASSERT_TRUE(marker.Value()->OnSessionStart().IsOk());
        // Simulate power loss: no OnCleanShutdown() call, object is
        // simply destroyed here as the process would vanish.
    }

    auto file2 = Storage::OpenFile(path, Storage::OpenMode::OpenExisting);
    ASSERT_TRUE(file2.IsOk());
    auto marker2 = PowerResilience::CreateSessionMarker(std::move(file2.Value()));
    ASSERT_TRUE(marker2.IsOk());

    auto state = marker2.Value()->ReadLastState();
    ASSERT_TRUE(state.IsOk());
    EXPECT_FALSE(state.Value().closedCleanly);
    EXPECT_EQ(state.Value().generation, 1u);
}

TEST_F(PowerResilienceTest, Journal_AppendThenReplay_ReturnsAllRecordsInOrder) {
    auto path = PathFor("journal.dat");
    auto file = Storage::OpenFile(path, Storage::OpenMode::OpenOrCreate);
    ASSERT_TRUE(file.IsOk());

    auto journal = PowerResilience::CreateWriteAheadJournal(std::move(file.Value()));
    ASSERT_TRUE(journal.IsOk());

    ASSERT_TRUE(journal.Value()->Append({1, 2, 3}).IsOk());
    ASSERT_TRUE(journal.Value()->Append({4, 5}).IsOk());
    ASSERT_TRUE(journal.Value()->Append({}).IsOk());

    std::vector<std::vector<std::uint8_t>> replayed;
    auto result = journal.Value()->Replay(
        [&](const PowerResilience::JournalRecord& record) {
            replayed.push_back(record.payload);
            return Common::Result<PowerResilience::ReplayAction>::Success(
                PowerResilience::ReplayAction::Continue);
        });

    ASSERT_TRUE(result.IsOk());
    ASSERT_EQ(replayed.size(), 3u);
    EXPECT_EQ(replayed[0], (std::vector<std::uint8_t>{1, 2, 3}));
    EXPECT_EQ(replayed[1], (std::vector<std::uint8_t>{4, 5}));
    EXPECT_TRUE(replayed[2].empty());
}

TEST_F(PowerResilienceTest, Journal_TornTailRecord_IsDiscardedNotMisinterpreted) {
    // Simulates the exact failure mode of a power cut mid-append: the
    // frame header is written but the FlushDurable()-guaranteed full
    // frame + trailing CRC never made it to disk. Replay must stop at
    // the tear and must NOT report a corrupted/partial record as valid.
    auto path = PathFor("journal_torn.dat");

    {
        auto file = Storage::OpenFile(path, Storage::OpenMode::OpenOrCreate);
        ASSERT_TRUE(file.IsOk());
        auto journal = PowerResilience::CreateWriteAheadJournal(std::move(file.Value()));
        ASSERT_TRUE(journal.IsOk());
        ASSERT_TRUE(journal.Value()->Append({10, 20, 30}).IsOk());
    }

    // Manually corrupt/truncate the file to simulate a torn write: append
    // a few extra bytes that look like the start of a new frame but are
    // incomplete (as if power was lost mid-Append of a second record).
    {
        std::FILE* fp = std::fopen((testDir_ / "journal_torn.dat").string().c_str(), "ab");
        ASSERT_NE(fp, nullptr);
        std::uint8_t garbage[6] = {0x31, 0x4A, 0x43, 0x51, 0x00, 0x00}; // partial frame magic + junk
        std::fwrite(garbage, 1, sizeof(garbage), fp);
        std::fclose(fp);
    }

    auto file2 = Storage::OpenFile(path, Storage::OpenMode::OpenExisting);
    ASSERT_TRUE(file2.IsOk());
    auto journal2 = PowerResilience::CreateWriteAheadJournal(std::move(file2.Value()));
    ASSERT_TRUE(journal2.IsOk());

    std::vector<std::vector<std::uint8_t>> replayed;
    auto result = journal2.Value()->Replay(
        [&](const PowerResilience::JournalRecord& record) {
            replayed.push_back(record.payload);
            return Common::Result<PowerResilience::ReplayAction>::Success(
                PowerResilience::ReplayAction::Continue);
        });

    ASSERT_TRUE(result.IsOk()) << "torn tail must be treated as end-of-usable-data, not an error";
    ASSERT_EQ(replayed.size(), 1u);
    EXPECT_EQ(replayed[0], (std::vector<std::uint8_t>{10, 20, 30}));
}

TEST_F(PowerResilienceTest, RecoveryManager_CleanShutdown_DoesNotInvokeReplayHandler) {
    auto markerFile = Storage::OpenFile(PathFor("m.dat"), Storage::OpenMode::OpenOrCreate);
    auto journalFile = Storage::OpenFile(PathFor("j.dat"), Storage::OpenMode::OpenOrCreate);
    ASSERT_TRUE(markerFile.IsOk());
    ASSERT_TRUE(journalFile.IsOk());

    auto marker = PowerResilience::CreateSessionMarker(std::move(markerFile.Value()));
    auto journal = PowerResilience::CreateWriteAheadJournal(std::move(journalFile.Value()));
    ASSERT_TRUE(marker.IsOk());
    ASSERT_TRUE(journal.IsOk());

    auto recoveryManager = PowerResilience::CreateRecoveryManager(
        std::move(marker.Value()), std::move(journal.Value()));
    ASSERT_TRUE(recoveryManager.IsOk());

    bool replayInvoked = false;
    auto initResult = recoveryManager.Value()->InitializeAndRecover(
        [&]() -> Common::Result<void> {
            replayInvoked = true;
            return Common::Result<void>::Success();
        });

    ASSERT_TRUE(initResult.IsOk());
    EXPECT_FALSE(replayInvoked);
    EXPECT_EQ(recoveryManager.Value()->CurrentState(), PowerResilience::RecoveryState::RecoveryComplete);
}

TEST_F(PowerResilienceTest, RecoveryManager_UncleanShutdown_InvokesReplayAndCompletesRecovery) {
    auto path = PathFor("m2.dat");

    // First "session": starts but never cleanly shuts down (power cut).
    {
        auto markerFile = Storage::OpenFile(path, Storage::OpenMode::OpenOrCreate);
        ASSERT_TRUE(markerFile.IsOk());
        auto marker = PowerResilience::CreateSessionMarker(std::move(markerFile.Value()));
        ASSERT_TRUE(marker.IsOk());
        ASSERT_TRUE(marker.Value()->OnSessionStart().IsOk());
    }

    auto markerFile2 = Storage::OpenFile(path, Storage::OpenMode::OpenExisting);
    auto journalFile2 = Storage::OpenFile(PathFor("j2.dat"), Storage::OpenMode::OpenOrCreate);
    ASSERT_TRUE(markerFile2.IsOk());
    ASSERT_TRUE(journalFile2.IsOk());

    auto marker2 = PowerResilience::CreateSessionMarker(std::move(markerFile2.Value()));
    auto journal2 = PowerResilience::CreateWriteAheadJournal(std::move(journalFile2.Value()));
    ASSERT_TRUE(marker2.IsOk());
    ASSERT_TRUE(journal2.IsOk());

    auto recoveryManager = PowerResilience::CreateRecoveryManager(
        std::move(marker2.Value()), std::move(journal2.Value()));
    ASSERT_TRUE(recoveryManager.IsOk());

    bool replayInvoked = false;
    auto initResult = recoveryManager.Value()->InitializeAndRecover(
        [&]() -> Common::Result<void> {
            replayInvoked = true;
            return Common::Result<void>::Success();
        });

    ASSERT_TRUE(initResult.IsOk());
    EXPECT_TRUE(replayInvoked);
    EXPECT_EQ(recoveryManager.Value()->CurrentState(), PowerResilience::RecoveryState::RecoveryComplete);
}

TEST_F(PowerResilienceTest, RecoveryManager_FailedReplay_ReportsRecoveryFailed) {
    auto path = PathFor("m3.dat");
    {
        auto markerFile = Storage::OpenFile(path, Storage::OpenMode::OpenOrCreate);
        ASSERT_TRUE(markerFile.IsOk());
        auto marker = PowerResilience::CreateSessionMarker(std::move(markerFile.Value()));
        ASSERT_TRUE(marker.IsOk());
        ASSERT_TRUE(marker.Value()->OnSessionStart().IsOk());
    }

    auto markerFile2 = Storage::OpenFile(path, Storage::OpenMode::OpenExisting);
    auto journalFile2 = Storage::OpenFile(PathFor("j3.dat"), Storage::OpenMode::OpenOrCreate);
    auto marker2 = PowerResilience::CreateSessionMarker(std::move(markerFile2.Value()));
    auto journal2 = PowerResilience::CreateWriteAheadJournal(std::move(journalFile2.Value()));

    auto recoveryManager = PowerResilience::CreateRecoveryManager(
        std::move(marker2.Value()), std::move(journal2.Value()));
    ASSERT_TRUE(recoveryManager.IsOk());

    auto initResult = recoveryManager.Value()->InitializeAndRecover(
        [&]() -> Common::Result<void> {
            return Common::Result<void>::Failure(
                Common::Error{Common::ErrorCode::JournalCorrupt, "simulated unrecoverable journal", 0});
        });

    EXPECT_FALSE(initResult.IsOk());
    EXPECT_EQ(recoveryManager.Value()->CurrentState(), PowerResilience::RecoveryState::RecoveryFailed);
}

// ---------------------------------------------------------------------
// SessionMarker CRC / corruption / truncation regression tests
// (Priority 1.4). These directly exercise the AUDITED BUG fix in
// SessionMarker::ReadLastState(): a CRC-invalid or torn record must
// NEVER have its generation/timestamp/closedCleanly fields trusted, and
// must NEVER be reported as a clean shutdown.
// ---------------------------------------------------------------------

namespace {
// Mirrors SessionMarker.cpp's private on-disk layout so tests can
// fabricate specific byte patterns (valid, truncated, corrupted, CRC-bad)
// without depending on SessionMarker's internals directly.
#pragma pack(push, 1)
struct TestRawMarkerRecord {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint64_t generation;
    std::uint64_t startTimestamp;
    std::uint8_t closedCleanly;
    std::uint32_t crc32;
};
#pragma pack(pop)
static_assert(sizeof(TestRawMarkerRecord) == 29, "must match SessionMarker.cpp's kRecordSize");

constexpr std::uint32_t kTestMarkerMagic = 0x51434D31u; // "QCM1"
constexpr std::uint32_t kTestMarkerVersion = 1u;

// Simple CRC32 (matches Common::Crc32 algorithm: standard IEEE 802.3
// polynomial). Re-implemented locally in the test so this file does not
// need to depend on Common::Crc32's header just for test fixtures — kept
// intentionally simple/obviously-correct rather than optimized.
std::uint32_t SimpleCrc32(const void* data, std::size_t length) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            std::uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

void WriteRawBytes(const fs::path& path, const void* data, std::size_t length) {
    std::FILE* fp = std::fopen(path.string().c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::fwrite(data, 1, length, fp);
    std::fclose(fp);
}
} // namespace

TEST_F(PowerResilienceTest, SessionMarker_ValidRecord_IsTrustedAsIs) {
    auto path = testDir_ / "marker_valid.dat";

    TestRawMarkerRecord raw{};
    raw.magic = kTestMarkerMagic;
    raw.version = kTestMarkerVersion;
    raw.generation = 42;
    raw.startTimestamp = 123456789;
    raw.closedCleanly = 1;
    raw.crc32 = SimpleCrc32(&raw, offsetof(TestRawMarkerRecord, crc32));
    WriteRawBytes(path, &raw, sizeof(raw));

    auto file = Storage::OpenFile(PathFor("marker_valid.dat"), Storage::OpenMode::OpenExisting);
    ASSERT_TRUE(file.IsOk());
    auto marker = PowerResilience::CreateSessionMarker(std::move(file.Value()));
    ASSERT_TRUE(marker.IsOk());

    auto state = marker.Value()->ReadLastState();
    ASSERT_TRUE(state.IsOk());
    EXPECT_EQ(state.Value().generation, 42u);
    EXPECT_EQ(state.Value().startTimestamp, 123456789u);
    EXPECT_TRUE(state.Value().closedCleanly);
}

TEST_F(PowerResilienceTest, SessionMarker_TruncatedRecord_IsUnclean_NotClaimedClean) {
    // A NON-ZERO but short file (fewer than kRecordSize bytes) means a
    // write was interrupted by a power cut before the record ever
    // completed. This must never be reported as "clean" — that would
    // silently skip journal replay for data written just before the cut.
    auto path = testDir_ / "marker_truncated.dat";

    TestRawMarkerRecord raw{};
    raw.magic = kTestMarkerMagic;
    raw.version = kTestMarkerVersion;
    raw.generation = 7;
    raw.startTimestamp = 999;
    raw.closedCleanly = 1; // even though the (fabricated) full record would say "clean"...
    raw.crc32 = SimpleCrc32(&raw, offsetof(TestRawMarkerRecord, crc32));
    // Only write the first 10 of 29 bytes, simulating a torn write.
    WriteRawBytes(path, &raw, 10);

    auto file = Storage::OpenFile(PathFor("marker_truncated.dat"), Storage::OpenMode::OpenExisting);
    ASSERT_TRUE(file.IsOk());
    auto marker = PowerResilience::CreateSessionMarker(std::move(file.Value()));
    ASSERT_TRUE(marker.IsOk());

    auto state = marker.Value()->ReadLastState();
    ASSERT_TRUE(state.IsOk());
    EXPECT_FALSE(state.Value().closedCleanly)
        << "a torn/truncated marker record must never be reported as a clean shutdown";
    EXPECT_EQ(state.Value().generation, 0u)
        << "generation from an untrusted/incomplete record must not leak through";
}

TEST_F(PowerResilienceTest, SessionMarker_FreshEmptyFile_IsReportedAsClean) {
    // A genuinely empty (zero-byte) file — the real "nothing has ever
    // been written here" case, e.g. very first run — is legitimately
    // safe to treat as clean, since there is no prior session to recover.
    auto path = testDir_ / "marker_empty.dat";
    { std::FILE* fp = std::fopen(path.string().c_str(), "wb"); ASSERT_NE(fp, nullptr); std::fclose(fp); }

    auto file = Storage::OpenFile(PathFor("marker_empty.dat"), Storage::OpenMode::OpenExisting);
    ASSERT_TRUE(file.IsOk());
    auto marker = PowerResilience::CreateSessionMarker(std::move(file.Value()));
    ASSERT_TRUE(marker.IsOk());

    auto state = marker.Value()->ReadLastState();
    ASSERT_TRUE(state.IsOk());
    EXPECT_TRUE(state.Value().closedCleanly);
    EXPECT_EQ(state.Value().generation, 0u);
}

TEST_F(PowerResilienceTest, SessionMarker_CorruptedPayload_CrcFailure_IsUnclean_FieldsNotTrusted) {
    // Full-size record, but a payload byte was flipped (e.g. a torn
    // write that landed mid-sector) so the CRC no longer matches. Fields
    // must be reported as unknown/untrusted, and the shutdown status
    // must be reported as unclean — regardless of what the corrupted
    // bytes happen to say.
    auto path = testDir_ / "marker_crc_bad.dat";

    TestRawMarkerRecord raw{};
    raw.magic = kTestMarkerMagic;
    raw.version = kTestMarkerVersion;
    raw.generation = 999;
    raw.startTimestamp = 555;
    raw.closedCleanly = 1;
    raw.crc32 = SimpleCrc32(&raw, offsetof(TestRawMarkerRecord, crc32));
    // Corrupt one payload byte after CRC was computed, without fixing up
    // the CRC — this is exactly what a torn/partial sector write looks
    // like: some new bytes made it, others didn't.
    raw.generation = 111222333;
    WriteRawBytes(path, &raw, sizeof(raw));

    auto file = Storage::OpenFile(PathFor("marker_crc_bad.dat"), Storage::OpenMode::OpenExisting);
    ASSERT_TRUE(file.IsOk());
    auto marker = PowerResilience::CreateSessionMarker(std::move(file.Value()));
    ASSERT_TRUE(marker.IsOk());

    auto state = marker.Value()->ReadLastState();
    ASSERT_TRUE(state.IsOk());
    EXPECT_FALSE(state.Value().closedCleanly);
    EXPECT_EQ(state.Value().generation, 0u)
        << "a CRC-invalid record's generation must never be surfaced, even though the corrupted "
           "bytes contain a value";
    EXPECT_EQ(state.Value().startTimestamp, 0u);
}

TEST_F(PowerResilienceTest, SessionMarker_CorruptedGenerationField_DoesNotPoisonNextSessionStart) {
    // This is the specific scenario the AUDITED BUG allowed: a
    // CRC-invalid record's (fabricated, huge) generation value used to
    // be fed directly into lastKnownGeneration_, so the NEXT
    // OnSessionStart() would increment from a fabricated number. Confirm
    // that after reading a corrupted record, a fresh OnSessionStart()
    // produces a well-defined, small (not-fabricated) generation.
    auto path = testDir_ / "marker_poison.dat";

    TestRawMarkerRecord raw{};
    raw.magic = kTestMarkerMagic;
    raw.version = kTestMarkerVersion;
    raw.generation = 0xFFFFFFFFFFFFFFFFull; // absurd sentinel the corrupted bytes might contain
    raw.startTimestamp = 0;
    raw.closedCleanly = 0;
    raw.crc32 = SimpleCrc32(&raw, offsetof(TestRawMarkerRecord, crc32));
    raw.crc32 ^= 0xFFFFFFFFu; // deliberately break the CRC so this is treated as corrupt
    WriteRawBytes(path, &raw, sizeof(raw));

    auto file = Storage::OpenFile(PathFor("marker_poison.dat"), Storage::OpenMode::OpenExisting);
    ASSERT_TRUE(file.IsOk());
    auto marker = PowerResilience::CreateSessionMarker(std::move(file.Value()));
    ASSERT_TRUE(marker.IsOk());

    auto state = marker.Value()->ReadLastState();
    ASSERT_TRUE(state.IsOk());
    EXPECT_FALSE(state.Value().closedCleanly);
    EXPECT_EQ(state.Value().generation, 0u);

    // Now start a new session: this must NOT continue counting from the
    // fabricated 0xFFFF...FFFF value (which would wrap/overflow or
    // otherwise produce nonsense); it must be exactly 1, since
    // lastKnownGeneration_ was never poisoned.
    ASSERT_TRUE(marker.Value()->OnSessionStart().IsOk());
    EXPECT_EQ(marker.Value()->CurrentGeneration(), 1u);
}

TEST_F(PowerResilienceTest, SessionMarker_RecoveryAfterCorruptedNewestRecord_TriggersReplay) {
    // End-to-end: a corrupted newest marker record must cause
    // RecoveryManager to treat the situation as an unclean shutdown and
    // invoke the journal replay handler, exactly as a real power cut
    // mid-write would require.
    auto path = testDir_ / "marker_e2e_corrupt.dat";

    TestRawMarkerRecord raw{};
    raw.magic = kTestMarkerMagic;
    raw.version = kTestMarkerVersion;
    raw.generation = 5;
    raw.startTimestamp = 42;
    raw.closedCleanly = 1; // corrupted bytes CLAIM clean, but CRC won't match
    raw.crc32 = SimpleCrc32(&raw, offsetof(TestRawMarkerRecord, crc32));
    raw.closedCleanly = 0; // flip after CRC computed -> CRC mismatch, simulating a torn write
    WriteRawBytes(path, &raw, sizeof(raw));

    auto markerFile = Storage::OpenFile(PathFor("marker_e2e_corrupt.dat"), Storage::OpenMode::OpenExisting);
    auto journalFile = Storage::OpenFile(PathFor("journal_e2e_corrupt.dat"), Storage::OpenMode::OpenOrCreate);
    ASSERT_TRUE(markerFile.IsOk());
    ASSERT_TRUE(journalFile.IsOk());

    auto marker = PowerResilience::CreateSessionMarker(std::move(markerFile.Value()));
    auto journal = PowerResilience::CreateWriteAheadJournal(std::move(journalFile.Value()));
    ASSERT_TRUE(marker.IsOk());
    ASSERT_TRUE(journal.IsOk());

    auto recoveryManager = PowerResilience::CreateRecoveryManager(
        std::move(marker.Value()), std::move(journal.Value()));
    ASSERT_TRUE(recoveryManager.IsOk());

    bool replayInvoked = false;
    auto initResult = recoveryManager.Value()->InitializeAndRecover(
        [&]() -> Common::Result<void> {
            replayInvoked = true;
            return Common::Result<void>::Success();
        });

    ASSERT_TRUE(initResult.IsOk());
    EXPECT_TRUE(replayInvoked)
        << "a corrupted (CRC-invalid) newest marker record, despite its corrupted bytes "
           "claiming a clean shutdown, must be treated as unclean and trigger replay";
    EXPECT_EQ(recoveryManager.Value()->CurrentState(), PowerResilience::RecoveryState::RecoveryComplete);
}

// ---------------------------------------------------------------------
// Priority 1.3 regression test: interruption between journal truncation
// (SetLength) and its durable flush.
//
// Truncate()'s real sequence is: SetLength(0) -> Seek(0) ->
// FlushDurable(). We cannot literally cut power mid-syscall from a
// portable unit test, but we CAN inject a failure exactly at the
// FlushDurable() step (simulating "the OS durability request never
// completed/was never reached before the crash") via a decorator around
// a real IFile, and verify the SAFETY PROPERTY that must hold regardless:
// Truncate() must report FAILURE (never silently claim success) when the
// durable flush of the truncation does not complete, so callers (e.g.
// CacheEngine::Shutdown) never mistakenly believe the journal was safely
// discarded.
// ---------------------------------------------------------------------
namespace {
class FlushDurableFailingFileDecorator final : public Storage::IFile {
public:
    explicit FlushDurableFailingFileDecorator(std::unique_ptr<Storage::IFile> inner)
        : inner_(std::move(inner)) {}

    Common::Result<std::size_t> Read(void* buffer, std::size_t bytes) override {
        return inner_->Read(buffer, bytes);
    }
    Common::Result<std::size_t> Write(const void* buffer, std::size_t bytes) override {
        return inner_->Write(buffer, bytes);
    }
    Common::Result<std::uint64_t> Seek(std::int64_t offset, bool fromEnd) override {
        return inner_->Seek(offset, fromEnd);
    }
    Common::Result<std::uint64_t> Size() const override { return inner_->Size(); }
    Common::Result<void> SetLength(std::uint64_t length) override {
        setLengthCallCount++;
        return inner_->SetLength(length);
    }
    Common::Result<void> FlushDurable() override {
        flushDurableCallCount++;
        if (failNextFlush) {
            failNextFlush = false;
            // Simulates a crash occurring after SetLength() has already
            // taken effect but before the durability request for that
            // truncation ever completed/was ever issued successfully.
            return Common::Result<void>::Failure(
                Common::Error{Common::ErrorCode::IoError,
                              "simulated power-loss interruption between truncate and durable flush",
                              0});
        }
        return inner_->FlushDurable();
    }
    void Close() override { inner_->Close(); }

    bool failNextFlush{false};
    int setLengthCallCount{0};
    int flushDurableCallCount{0};

private:
    std::unique_ptr<Storage::IFile> inner_;
};
} // namespace

TEST_F(PowerResilienceTest, Journal_Truncate_InterruptedBeforeDurableFlush_ReportsFailure_NotSilentSuccess) {
    auto path = PathFor("journal_trunc_interrupt.dat");
    auto file = Storage::OpenFile(path, Storage::OpenMode::OpenOrCreate);
    ASSERT_TRUE(file.IsOk());

    auto* decoratorPtr = new FlushDurableFailingFileDecorator(std::move(file.Value()));
    auto journal = PowerResilience::CreateWriteAheadJournal(
        std::unique_ptr<Storage::IFile>(decoratorPtr));
    ASSERT_TRUE(journal.IsOk());

    ASSERT_TRUE(journal.Value()->Append({1, 2, 3}).IsOk());
    ASSERT_TRUE(journal.Value()->Append({4, 5, 6}).IsOk());
    ASSERT_EQ(journal.Value()->RecordCount(), 2u);

    // Now simulate the crash landing exactly between SetLength(0) and the
    // durable flush that is supposed to make that truncation crash-safe.
    decoratorPtr->failNextFlush = true;
    auto truncateResult = journal.Value()->Truncate();

    // The critical safety property: Truncate() must surface this as a
    // real failure, never as success. A caller that silently treated
    // this as "journal safely discarded" could go on to also discard
    // records that were never durably confirmed as truncated.
    EXPECT_FALSE(truncateResult.IsOk())
        << "an interrupted (non-durable) truncation must never be reported as successful";

    // AND: the in-memory RecordCount() must NOT have been optimistically
    // reset to 0 on a failed truncate — the object's own bookkeeping
    // must stay consistent with "truncation not confirmed durable",
    // matching WriteAheadJournal.cpp's actual (correct) implementation
    // ordering (recordCount_ is only zeroed AFTER FlushDurable succeeds).
    EXPECT_EQ(journal.Value()->RecordCount(), 2u)
        << "RecordCount() must not silently drop to 0 when the durable flush step failed";
}

TEST_F(PowerResilienceTest, Journal_Truncate_SucceedsNormally_AfterPriorInterruptedAttempt) {
    // A failed truncate attempt must not permanently wedge the journal:
    // a subsequent real Truncate() call (as would happen on a retry,
    // e.g. the next successful shutdown) must still be able to succeed.
    auto path = PathFor("journal_trunc_retry.dat");
    auto file = Storage::OpenFile(path, Storage::OpenMode::OpenOrCreate);
    ASSERT_TRUE(file.IsOk());

    auto* decoratorPtr = new FlushDurableFailingFileDecorator(std::move(file.Value()));
    auto journal = PowerResilience::CreateWriteAheadJournal(
        std::unique_ptr<Storage::IFile>(decoratorPtr));
    ASSERT_TRUE(journal.IsOk());

    ASSERT_TRUE(journal.Value()->Append({9, 9, 9}).IsOk());

    decoratorPtr->failNextFlush = true;
    EXPECT_FALSE(journal.Value()->Truncate().IsOk());

    // Retry without the injected failure: must succeed and genuinely
    // reset the journal this time.
    EXPECT_TRUE(journal.Value()->Truncate().IsOk());
    EXPECT_EQ(journal.Value()->RecordCount(), 0u);
}

TEST_F(PowerResilienceTest, Journal_CorruptedHugeLengthField_IsRejectedSafely_NoOOMAttempt) {
    // AUDITED BUG (fixed) regression test: a corrupted payloadLength
    // field (e.g. a single bit-flip turning a small real length into
    // something enormous) must be treated as a torn/corrupt frame and
    // safely discarded by Replay(), never attempted as a real
    // allocation. Simulates this by hand-crafting a frame whose header
    // claims a huge payload length that could never legitimately exist
    // in a frame this short.
    auto path = PathFor("journal_huge_length.dat");

    {
        auto file = Storage::OpenFile(path, Storage::OpenMode::OpenOrCreate);
        ASSERT_TRUE(file.IsOk());
        auto journal = PowerResilience::CreateWriteAheadJournal(std::move(file.Value()));
        ASSERT_TRUE(journal.IsOk());
        // One genuinely valid record first, so we can confirm it is
        // still correctly recovered despite the corrupted record after it.
        ASSERT_TRUE(journal.Value()->Append({7, 8, 9}).IsOk());
    }

    // Hand-craft a second, corrupted frame directly on disk: valid magic
    // + sequence, but an absurd payloadLength (close to UINT32_MAX).
    {
        std::FILE* fp = std::fopen((testDir_ / "journal_huge_length.dat").string().c_str(), "ab");
        ASSERT_NE(fp, nullptr);
        std::uint32_t frameMagic = 0x51434A31u; // "QCJ1", matches WriteAheadJournal.cpp's kFrameMagic
        std::uint64_t sequenceNumber = 1;
        std::uint32_t hugeLength = 0xFFFFFFF0u; // ~4 GiB, absurd for a real record
        std::fwrite(&frameMagic, sizeof(frameMagic), 1, fp);
        std::fwrite(&sequenceNumber, sizeof(sequenceNumber), 1, fp);
        std::fwrite(&hugeLength, sizeof(hugeLength), 1, fp);
        std::fclose(fp);
    }

    auto file2 = Storage::OpenFile(path, Storage::OpenMode::OpenExisting);
    ASSERT_TRUE(file2.IsOk());
    auto journal2 = PowerResilience::CreateWriteAheadJournal(std::move(file2.Value()));
    ASSERT_TRUE(journal2.IsOk());

    std::vector<std::vector<std::uint8_t>> replayed;
    auto result = journal2.Value()->Replay(
        [&](const PowerResilience::JournalRecord& record) {
            replayed.push_back(record.payload);
            return Common::Result<PowerResilience::ReplayAction>::Success(
                PowerResilience::ReplayAction::Continue);
        });

    // Must complete without crashing/OOM-ing and without treating the
    // corrupted record as valid; the one real record before it must
    // still be recovered.
    ASSERT_TRUE(result.IsOk())
        << "a corrupted huge-length record must be treated like a torn tail, not a hard error";
    ASSERT_EQ(replayed.size(), 1u);
    EXPECT_EQ(replayed[0], (std::vector<std::uint8_t>{7, 8, 9}));
}

TEST_F(PowerResilienceTest, Journal_Truncate_ActuallyShrinksFileToZeroBytes) {
    // Stage 2 fix: Truncate() now really calls IFile::SetLength(0) rather
    // than only resetting in-memory bookkeeping (Stage 1's documented
    // gap). This matters for journal compaction after the cache engine
    // durably applies flushed records to the backing store.
    auto path = PathFor("journal_truncate.dat");

    // AUDITED FINDING (real Windows-runtime behavior, discovered via
    // Wine execution — see docs/ENVIRONMENT.md "Windows runtime
    // testing"): the first IFile handle must be fully closed (scope
    // exit here) before a second handle to the same path is opened
    // below — Win32File opens with only FILE_SHARE_READ, so an
    // overlapping GENERIC_WRITE handle to the same file fails with a
    // real Windows sharing violation. This is a test-authoring
    // requirement (matches every production call site), not a product
    // bug.
    {
        auto file = Storage::OpenFile(path, Storage::OpenMode::OpenOrCreate);
        ASSERT_TRUE(file.IsOk());
        auto journal = PowerResilience::CreateWriteAheadJournal(std::move(file.Value()));
        ASSERT_TRUE(journal.IsOk());

        ASSERT_TRUE(journal.Value()->Append({1, 2, 3, 4, 5}).IsOk());
        ASSERT_TRUE(journal.Value()->Append({6, 7, 8}).IsOk());
        EXPECT_EQ(journal.Value()->RecordCount(), 2u);

        ASSERT_TRUE(journal.Value()->Truncate().IsOk());
        EXPECT_EQ(journal.Value()->RecordCount(), 0u);
    }

    // Reopen fresh and confirm the file is really zero bytes on disk, not
    // just logically reset in the live object.
    auto file2 = Storage::OpenFile(path, Storage::OpenMode::OpenExisting);
    ASSERT_TRUE(file2.IsOk());
    auto sizeResult = file2.Value()->Size();
    ASSERT_TRUE(sizeResult.IsOk());
    EXPECT_EQ(sizeResult.Value(), 0u);

    // And replay on the reopened, truncated journal yields no records.
    auto journal2 = PowerResilience::CreateWriteAheadJournal(std::move(file2.Value()));
    ASSERT_TRUE(journal2.IsOk());
    int replayedCount = 0;
    auto replayResult = journal2.Value()->Replay(
        [&](const PowerResilience::JournalRecord&) {
            ++replayedCount;
            return Common::Result<PowerResilience::ReplayAction>::Success(
                PowerResilience::ReplayAction::Continue);
        });
    ASSERT_TRUE(replayResult.IsOk());
    EXPECT_EQ(replayedCount, 0);
}
