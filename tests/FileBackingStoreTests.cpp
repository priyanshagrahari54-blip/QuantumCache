// Direct unit coverage for Storage::FileBackingStore. Prior to this
// file, FileBackingStore was only exercised indirectly through
// CacheEngine tests (which go through CacheEngine's own API, not the
// backing store's raw Get/Put/Remove/Contains/EntryCount surface, and
// never exercise its on-disk corruption-recovery scan directly).
#include "QuantumCache/Storage/IBackingStore.h"
#include "QuantumCache/Storage/IFile.h"
#include <gtest/gtest.h>
#include <cstdio>
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

class FileBackingStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        testDir_ = fs::temp_directory_path() /
                   ("qc_backing_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
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

TEST_F(FileBackingStoreTest, PutThenGet_RoundTripsExactly) {
    auto store = Storage::OpenFileBackingStore(PathFor("basic.dat"));
    ASSERT_TRUE(store.IsOk());

    ASSERT_TRUE(store.Value()->Put("k1", Bytes("v1")).IsOk());
    auto result = store.Value()->Get("k1");
    ASSERT_TRUE(result.IsOk());
    EXPECT_EQ(ToStr(result.Value()), "v1");
}

TEST_F(FileBackingStoreTest, Get_MissingKey_ReturnsNotFound) {
    auto store = Storage::OpenFileBackingStore(PathFor("missing.dat"));
    ASSERT_TRUE(store.IsOk());
    auto result = store.Value()->Get("nope");
    EXPECT_FALSE(result.IsOk());
    EXPECT_EQ(result.Err().code, Common::ErrorCode::NotFound);
}

TEST_F(FileBackingStoreTest, Put_SameKeyTwice_LatestValueWins) {
    auto store = Storage::OpenFileBackingStore(PathFor("overwrite.dat"));
    ASSERT_TRUE(store.IsOk());
    ASSERT_TRUE(store.Value()->Put("k", Bytes("old")).IsOk());
    ASSERT_TRUE(store.Value()->Put("k", Bytes("new")).IsOk());
    auto result = store.Value()->Get("k");
    ASSERT_TRUE(result.IsOk());
    EXPECT_EQ(ToStr(result.Value()), "new");
}

TEST_F(FileBackingStoreTest, Remove_ThenGet_ReturnsNotFound) {
    auto store = Storage::OpenFileBackingStore(PathFor("remove.dat"));
    ASSERT_TRUE(store.IsOk());
    ASSERT_TRUE(store.Value()->Put("k", Bytes("v")).IsOk());
    ASSERT_TRUE(store.Value()->Remove("k").IsOk());
    EXPECT_FALSE(store.Value()->Contains("k"));
    auto result = store.Value()->Get("k");
    EXPECT_FALSE(result.IsOk());
    EXPECT_EQ(result.Err().code, Common::ErrorCode::NotFound);
}

TEST_F(FileBackingStoreTest, Remove_AbsentKey_IsIdempotent_NotAnError) {
    auto store = Storage::OpenFileBackingStore(PathFor("remove_absent.dat"));
    ASSERT_TRUE(store.IsOk());
    // Removing a key that was never present must succeed (used by
    // journal replay's Invalidate path, which must be safely repeatable
    // regardless of whether the original removal already landed before
    // a crash).
    EXPECT_TRUE(store.Value()->Remove("never-existed").IsOk());
}

TEST_F(FileBackingStoreTest, Contains_ReflectsCurrentState) {
    auto store = Storage::OpenFileBackingStore(PathFor("contains.dat"));
    ASSERT_TRUE(store.IsOk());
    EXPECT_FALSE(store.Value()->Contains("k"));
    ASSERT_TRUE(store.Value()->Put("k", Bytes("v")).IsOk());
    EXPECT_TRUE(store.Value()->Contains("k"));
    ASSERT_TRUE(store.Value()->Remove("k").IsOk());
    EXPECT_FALSE(store.Value()->Contains("k"));
}

TEST_F(FileBackingStoreTest, EntryCount_TracksDistinctLiveKeys) {
    auto store = Storage::OpenFileBackingStore(PathFor("count.dat"));
    ASSERT_TRUE(store.IsOk());
    ASSERT_TRUE(store.Value()->Put("a", Bytes("1")).IsOk());
    ASSERT_TRUE(store.Value()->Put("b", Bytes("2")).IsOk());
    ASSERT_TRUE(store.Value()->Put("a", Bytes("updated")).IsOk()); // overwrite, not a new entry
    EXPECT_EQ(store.Value()->EntryCount(), 2u);
    ASSERT_TRUE(store.Value()->Remove("a").IsOk());
    EXPECT_EQ(store.Value()->EntryCount(), 1u);
}

TEST_F(FileBackingStoreTest, ReopenAfterCleanClose_DataSurvivesAndIndexRebuildsCorrectly) {
    auto path = PathFor("reopen.dat");
    {
        auto store = Storage::OpenFileBackingStore(path);
        ASSERT_TRUE(store.IsOk());
        ASSERT_TRUE(store.Value()->Put("persisted", Bytes("value")).IsOk());
        ASSERT_TRUE(store.Value()->Put("removed", Bytes("gone")).IsOk());
        ASSERT_TRUE(store.Value()->Remove("removed").IsOk());
    }

    auto store2 = Storage::OpenFileBackingStore(path);
    ASSERT_TRUE(store2.IsOk());
    auto result = store2.Value()->Get("persisted");
    ASSERT_TRUE(result.IsOk());
    EXPECT_EQ(ToStr(result.Value()), "value");
    EXPECT_FALSE(store2.Value()->Contains("removed"));
}

TEST_F(FileBackingStoreTest, TornTailRecord_IsDiscardedSafely_PriorRecordsSurvive) {
    // Simulates a power cut mid-append of a new record: the previous
    // record is fully valid; the new one is an incomplete fragment.
    auto path = PathFor("torn.dat");
    {
        auto store = Storage::OpenFileBackingStore(path);
        ASSERT_TRUE(store.IsOk());
        ASSERT_TRUE(store.Value()->Put("good", Bytes("good-value")).IsOk());
    }

    {
        std::FILE* fp = std::fopen((testDir_ / "torn.dat").string().c_str(), "ab");
        ASSERT_NE(fp, nullptr);
        std::uint8_t garbage[10] = {0x53, 0x42, 0x43, 0x51, 0, 0, 0, 0, 0, 0}; // partial magic + junk
        std::fwrite(garbage, 1, sizeof(garbage), fp);
        std::fclose(fp);
    }

    // AUDITED FINDING (real Windows-runtime behavior, discovered via
    // Wine execution — see docs/ENVIRONMENT.md "Windows runtime
    // testing"): each IBackingStore/IFile handle must be fully scoped
    // and closed before opening a NEW handle to the same path — Win32
    // opens with only FILE_SHARE_READ (correct, intentional
    // single-writer semantics), so overlapping handles to the same file
    // for read/write access fail with a real Windows sharing violation.
    // This is a test-authoring requirement (matches every production
    // call site's pattern), not a product bug.
    {
        auto store2 = Storage::OpenFileBackingStore(path);
        ASSERT_TRUE(store2.IsOk());
        auto result = store2.Value()->Get("good");
        ASSERT_TRUE(result.IsOk());
        EXPECT_EQ(ToStr(result.Value()), "good-value");
        ASSERT_TRUE(store2.Value()->Put("second", Bytes("second-value")).IsOk());
    }

    // Confirm no leftover garbage bytes remain appended past the valid
    // record in a way that would corrupt subsequent reads/writes (exact
    // byte count depends on framing, so just confirm both records
    // round-trip correctly rather than hardcoding an exact size).
    {
        auto store3 = Storage::OpenFileBackingStore(path);
        ASSERT_TRUE(store3.IsOk());
        auto r1 = store3.Value()->Get("good");
        auto r2 = store3.Value()->Get("second");
        ASSERT_TRUE(r1.IsOk());
        ASSERT_TRUE(r2.IsOk());
        EXPECT_EQ(ToStr(r1.Value()), "good-value");
        EXPECT_EQ(ToStr(r2.Value()), "second-value");
    }
}

TEST_F(FileBackingStoreTest, CorruptedHugeLengthField_IsRejectedSafely_NoOOMAttempt) {
    // AUDITED BUG (fixed) regression test: a corrupted keyLength or
    // valueLength field (e.g. a bit-flip on disk) must never be used to
    // drive a raw allocation — see FileBackingStore.cpp's
    // kMaxReasonableLength comment. Simulates this via a hand-crafted
    // record header claiming an absurd valueLength.
    auto path = PathFor("huge_length.dat");
    {
        auto store = Storage::OpenFileBackingStore(path);
        ASSERT_TRUE(store.IsOk());
        ASSERT_TRUE(store.Value()->Put("good", Bytes("good-value")).IsOk());
    }

    {
        std::FILE* fp = std::fopen((testDir_ / "huge_length.dat").string().c_str(), "ab");
        ASSERT_NE(fp, nullptr);
        std::uint32_t magic = 0x51434253u; // "QCBS", matches FileBackingStore.cpp's kMagic
        std::uint64_t sequence = 99;
        std::uint8_t tombstone = 0;
        std::uint32_t keyLength = 3;
        std::fwrite(&magic, sizeof(magic), 1, fp);
        std::fwrite(&sequence, sizeof(sequence), 1, fp);
        std::fwrite(&tombstone, sizeof(tombstone), 1, fp);
        std::fwrite(&keyLength, sizeof(keyLength), 1, fp);
        std::fwrite("bad", 1, 3, fp);
        std::uint32_t hugeValueLength = 0xFFFFFFF0u; // ~4 GiB, absurd
        std::fwrite(&hugeValueLength, sizeof(hugeValueLength), 1, fp);
        std::fclose(fp);
    }

    // Must open successfully (treating the corrupted record as a torn
    // tail to discard) rather than crashing/OOM-ing/hanging.
    auto store2 = Storage::OpenFileBackingStore(path);
    ASSERT_TRUE(store2.IsOk());
    auto result = store2.Value()->Get("good");
    ASSERT_TRUE(result.IsOk());
    EXPECT_EQ(ToStr(result.Value()), "good-value");
    EXPECT_FALSE(store2.Value()->Contains("bad"));
}

TEST_F(FileBackingStoreTest, EmptyValue_RoundTripsCorrectly) {
    auto store = Storage::OpenFileBackingStore(PathFor("empty_value.dat"));
    ASSERT_TRUE(store.IsOk());
    ASSERT_TRUE(store.Value()->Put("k", std::vector<std::uint8_t>{}).IsOk());
    auto result = store.Value()->Get("k");
    ASSERT_TRUE(result.IsOk());
    EXPECT_TRUE(result.Value().empty());
}
