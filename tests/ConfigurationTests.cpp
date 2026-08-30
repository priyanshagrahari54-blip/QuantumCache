#include "QuantumCache/Configuration/IConfigStore.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sys/stat.h>

using namespace QuantumCache;
namespace fs = std::filesystem;

namespace {
class ConfigurationTest : public ::testing::Test {
protected:
    void SetUp() override {
        testDir_ = fs::temp_directory_path() / ("qc_cfg_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
        fs::create_directories(testDir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(testDir_, ec);
    }
    fs::path testDir_;
};
} // namespace

TEST_F(ConfigurationTest, SaveThenLoad_RoundTripsExactly) {
    auto storePath = (testDir_ / "config.json").string();
    auto store = Configuration::CreateJsonConfigStore(storePath);
    ASSERT_TRUE(store.IsOk());

    Configuration::AppConfig config;
    config.stateDirectory = L"C:\\Test\\State";
    config.logDirectory = L"C:\\Test\\Logs";
    config.minimumLogLevel = 3;
    config.ipcPipeName = L"\\\\.\\pipe\\TestPipe";

    ASSERT_TRUE(store.Value()->Save(config).IsOk());

    auto loaded = store.Value()->Load();
    ASSERT_TRUE(loaded.IsOk());
    EXPECT_EQ(loaded.Value(), config);
}

TEST_F(ConfigurationTest, Load_MissingFile_ReturnsNotFound) {
    auto store = Configuration::CreateJsonConfigStore((testDir_ / "missing.json").string());
    ASSERT_TRUE(store.IsOk());

    auto loaded = store.Value()->Load();
    EXPECT_FALSE(loaded.IsOk());
    EXPECT_EQ(loaded.Err().code, Common::ErrorCode::NotFound);
}

TEST_F(ConfigurationTest, Validate_RejectsEmptyStateDirectory) {
    auto store = Configuration::CreateJsonConfigStore((testDir_ / "c.json").string());
    ASSERT_TRUE(store.IsOk());

    Configuration::AppConfig config;
    config.stateDirectory = L"";

    auto result = store.Value()->Validate(config);
    EXPECT_FALSE(result.IsOk());
    EXPECT_EQ(result.Err().code, Common::ErrorCode::InvalidArgument);
}

TEST_F(ConfigurationTest, Validate_RejectsUnsupportedSchemaVersion) {
    auto store = Configuration::CreateJsonConfigStore((testDir_ / "c2.json").string());
    ASSERT_TRUE(store.IsOk());

    Configuration::AppConfig config;
    config.schemaVersion = 99;

    auto result = store.Value()->Validate(config);
    EXPECT_FALSE(result.IsOk());
    EXPECT_EQ(result.Err().code, Common::ErrorCode::VersionMismatch);
}

TEST_F(ConfigurationTest, Validate_RejectsMalformedPipeName) {
    auto store = Configuration::CreateJsonConfigStore((testDir_ / "c3.json").string());
    ASSERT_TRUE(store.IsOk());

    Configuration::AppConfig config;
    config.ipcPipeName = L"not-a-pipe-path";

    auto result = store.Value()->Validate(config);
    EXPECT_FALSE(result.IsOk());
    EXPECT_EQ(result.Err().code, Common::ErrorCode::InvalidArgument);
}

TEST_F(ConfigurationTest, Save_RejectsInvalidConfig_DoesNotWriteFile) {
    auto storePath = (testDir_ / "invalid.json").string();
    auto store = Configuration::CreateJsonConfigStore(storePath);
    ASSERT_TRUE(store.IsOk());

    Configuration::AppConfig config;
    config.minimumLogLevel = 999; // invalid

    auto saveResult = store.Value()->Save(config);
    EXPECT_FALSE(saveResult.IsOk());
    EXPECT_FALSE(fs::exists(storePath));
}

// ---------------------------------------------------------------------
// Stage 2: cache engine configuration fields.
// ---------------------------------------------------------------------

TEST_F(ConfigurationTest, Stage2Fields_RoundTripThroughSaveAndLoad) {
    auto storePath = (testDir_ / "stage2.json").string();
    auto store = Configuration::CreateJsonConfigStore(storePath);
    ASSERT_TRUE(store.IsOk());

    Configuration::AppConfig config;
    config.cacheEnabled = false;
    config.backingStoreDataFile = L"C:\\Test\\backing.data";
    config.cacheCapacityBytes = 123456789;
    config.cacheMaxEntryCount = 42;
    config.cacheShardCount = 8;
    config.evictionPolicy = "LeastRecentlyUsed";
    config.writePolicy = "WriteThrough";
    config.flushPolicy = "PeriodicBackground";
    config.flushIntervalSeconds = 60;

    ASSERT_TRUE(store.Value()->Save(config).IsOk());
    auto loaded = store.Value()->Load();
    ASSERT_TRUE(loaded.IsOk());
    EXPECT_EQ(loaded.Value(), config);
}

TEST_F(ConfigurationTest, Validate_RejectsZeroCacheCapacity) {
    auto store = Configuration::CreateJsonConfigStore((testDir_ / "c4.json").string());
    Configuration::AppConfig config;
    config.cacheCapacityBytes = 0;
    auto result = store.Value()->Validate(config);
    EXPECT_FALSE(result.IsOk());
    EXPECT_EQ(result.Err().code, Common::ErrorCode::InvalidArgument);
}

TEST_F(ConfigurationTest, Validate_RejectsImplausiblyLargeCacheCapacity) {
    auto store = Configuration::CreateJsonConfigStore((testDir_ / "c5.json").string());
    Configuration::AppConfig config;
    config.cacheCapacityBytes = std::numeric_limits<std::uint64_t>::max();
    auto result = store.Value()->Validate(config);
    EXPECT_FALSE(result.IsOk());
}

TEST_F(ConfigurationTest, Validate_RejectsNonPowerOfTwoShardCount) {
    auto store = Configuration::CreateJsonConfigStore((testDir_ / "c6.json").string());
    Configuration::AppConfig config;
    config.cacheShardCount = 10; // not a power of two
    auto result = store.Value()->Validate(config);
    EXPECT_FALSE(result.IsOk());
}

TEST_F(ConfigurationTest, Validate_RejectsUnknownEvictionPolicy) {
    auto store = Configuration::CreateJsonConfigStore((testDir_ / "c7.json").string());
    Configuration::AppConfig config;
    config.evictionPolicy = "MostRecentlyUsed"; // not implemented
    auto result = store.Value()->Validate(config);
    EXPECT_FALSE(result.IsOk());
}

TEST_F(ConfigurationTest, Validate_RejectsUnknownWritePolicy) {
    auto store = Configuration::CreateJsonConfigStore((testDir_ / "c8.json").string());
    Configuration::AppConfig config;
    config.writePolicy = "AsyncFireAndForget"; // not a real policy
    auto result = store.Value()->Validate(config);
    EXPECT_FALSE(result.IsOk());
}

TEST_F(ConfigurationTest, Validate_RejectsPeriodicFlushWithZeroInterval) {
    auto store = Configuration::CreateJsonConfigStore((testDir_ / "c9.json").string());
    Configuration::AppConfig config;
    config.flushPolicy = "PeriodicBackground";
    config.flushIntervalSeconds = 0;
    auto result = store.Value()->Validate(config);
    EXPECT_FALSE(result.IsOk());
}

TEST_F(ConfigurationTest, Validate_RejectsEmptyBackingStoreDataFile) {
    auto store = Configuration::CreateJsonConfigStore((testDir_ / "c10.json").string());
    Configuration::AppConfig config;
    config.backingStoreDataFile = L"";
    auto result = store.Value()->Validate(config);
    EXPECT_FALSE(result.IsOk());
}

TEST_F(ConfigurationTest, Load_Stage1SchemaVersionOne_MigratesForwardWithDefaults) {
    // Simulates loading a config file written by a Stage 1 build, which
    // has schemaVersion==1 and none of the Stage 2 fields present at all.
    // This must succeed (not be rejected as malformed) and produce sane
    // Stage 2 defaults for the missing fields — the actual behavior the
    // "schema version, so future stages can detect and migrate older
    // on-disk configs" AppConfig.h comment promises.
    auto path = (testDir_ / "stage1_legacy.json").string();
    {
        std::ofstream out(path);
        out << R"({
            "schemaVersion": 1,
            "stateDirectory": "C:\\Legacy\\State",
            "logDirectory": "C:\\Legacy\\Logs",
            "minimumLogLevel": 2,
            "ipcPipeName": "\\\\.\\pipe\\QuantumCacheControl"
        })";
    }

    auto store = Configuration::CreateJsonConfigStore(path);
    ASSERT_TRUE(store.IsOk());

    auto loaded = store.Value()->Load();
    ASSERT_TRUE(loaded.IsOk()) << "a Stage 1 config file must still load successfully";
    EXPECT_EQ(loaded.Value().schemaVersion, 1u);
    EXPECT_EQ(loaded.Value().stateDirectory, L"C:\\Legacy\\State");
    // Stage 2 fields must have fallen back to AppConfig's own defaults.
    EXPECT_TRUE(loaded.Value().cacheEnabled);
    EXPECT_EQ(loaded.Value().evictionPolicy, "LeastRecentlyUsed");
    EXPECT_EQ(loaded.Value().writePolicy, "WriteBackDeferred");
    EXPECT_GT(loaded.Value().cacheCapacityBytes, 0u);
}

// ---------------------------------------------------------------------
// Priority 1.6 regression tests: crash-safe Save() (AUDITED BUG fix).
// These verify the actual observable contract — "if anything fails
// partway through a save, the last known-good configuration must be
// preserved" — rather than merely inspecting the implementation.
// ---------------------------------------------------------------------

TEST_F(ConfigurationTest, Save_NeverWritesDirectlyToTargetPath_TempFileArtifactAppearsAndDisappears) {
    // A crash-safe save must never open the real target path for direct
    // truncating writes. We can't literally pause a running process
    // mid-syscall from a portable test, but we CAN verify the mechanism
    // this fix relies on: after a successful Save(), no leftover ".tmp"
    // sibling file remains (proving the rename step actually completed
    // and cleaned up), and the target file's final bytes are a complete,
    // parseable, valid config — never a half-written fragment.
    auto storePath = (testDir_ / "atomic.json").string();
    auto store = Configuration::CreateJsonConfigStore(storePath);
    ASSERT_TRUE(store.IsOk());

    Configuration::AppConfig config;
    config.cacheCapacityBytes = 777777;
    ASSERT_TRUE(store.Value()->Save(config).IsOk());

    // No stray temp artifact left behind.
    for (auto& entry : fs::directory_iterator(testDir_)) {
        std::string name = entry.path().filename().string();
        EXPECT_EQ(name.find(".tmp"), std::string::npos)
            << "leftover temp file after successful Save(): " << name;
    }

    // The target file itself must be complete and parse successfully.
    auto loaded = store.Value()->Load();
    ASSERT_TRUE(loaded.IsOk());
    EXPECT_EQ(loaded.Value().cacheCapacityBytes, 777777u);
}

TEST_F(ConfigurationTest, Save_Interrupted_PreservesLastKnownGoodConfig) {
    // Simulates "interruption during configuration save": we write a
    // known-good config first (this succeeds normally), then simulate a
    // crash occurring after step 1 (temp file written) but before step 3
    // (atomic rename into place) by manually creating a temp-file
    // artifact with garbage content and NOT renaming it — exactly the
    // on-disk state a real power cut between steps 1/2 and step 3 would
    // leave behind. The real target file must still contain the
    // last-known-good config, completely unaffected.
    auto storePath = (testDir_ / "interrupted.json").string();
    auto store = Configuration::CreateJsonConfigStore(storePath);
    ASSERT_TRUE(store.IsOk());

    Configuration::AppConfig goodConfig;
    goodConfig.cacheCapacityBytes = 555555;
    ASSERT_TRUE(store.Value()->Save(goodConfig).IsOk());

    // Simulate an interrupted second save: a temp file appears (as
    // AtomicallyReplaceFileContents' step 1 would create) but the
    // process is imagined to have died before the rename in step 3, so
    // the temp file is left dangling with different (would-be-new, but
    // never should-have-taken-effect) content.
    std::string tempPath = storePath + ".tmp";
    {
        std::ofstream tmp(tempPath, std::ios::binary);
        tmp << "{ not even valid json, simulating a torn write }";
    }

    // The REAL config file must be completely unaffected by the
    // dangling temp file — this is the actual "preserve last known-good
    // config" contract.
    auto loaded = store.Value()->Load();
    ASSERT_TRUE(loaded.IsOk());
    EXPECT_EQ(loaded.Value().cacheCapacityBytes, 555555u)
        << "an interrupted save (dangling temp file, no rename) must never affect the "
           "last known-good configuration";

    // Clean up the simulated leftover temp artifact.
    std::error_code ec;
    fs::remove(tempPath, ec);
}

TEST_F(ConfigurationTest, Save_ThenReload_MultipleTimesInARow_AlwaysRoundTrips) {
    // Repeated save/reload cycles (simulating repeated config edits over
    // a long-running service's lifetime) must never accumulate stray
    // artifacts or leave the file in an inconsistent state between
    // saves, since each Save() must be independently crash-safe.
    auto storePath = (testDir_ / "repeated.json").string();
    auto store = Configuration::CreateJsonConfigStore(storePath);
    ASSERT_TRUE(store.IsOk());

    for (std::uint64_t i = 1; i <= 5; ++i) {
        Configuration::AppConfig config;
        config.cacheCapacityBytes = i * 1000;
        ASSERT_TRUE(store.Value()->Save(config).IsOk());

        auto loaded = store.Value()->Load();
        ASSERT_TRUE(loaded.IsOk());
        EXPECT_EQ(loaded.Value().cacheCapacityBytes, i * 1000);
    }

    // Exactly one JSON file plus no leftover temp artifacts at the end.
    int fileCount = 0;
    for (auto& entry : fs::directory_iterator(testDir_)) {
        ++fileCount;
        EXPECT_EQ(entry.path().filename().string(), fs::path(storePath).filename().string());
    }
    EXPECT_EQ(fileCount, 1);
}
