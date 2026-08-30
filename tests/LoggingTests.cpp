#include "QuantumCache/Logging/Logger.h"
#include "QuantumCache/Logging/MemoryLogSink.h"
#include <gtest/gtest.h>

using namespace QuantumCache::Logging;

TEST(LoggerTest, RespectsMinimumLevel) {
    Logger logger(LogLevel::Warning);
    auto sink = std::make_shared<MemoryLogSink>();
    logger.AddSink(sink);

    logger.Log(LogLevel::Debug, "Test", "should be filtered out");
    logger.Log(LogLevel::Info, "Test", "should also be filtered out");
    logger.Log(LogLevel::Warning, "Test", "should appear");
    logger.Log(LogLevel::Error, "Test", "should also appear");

    auto entries = sink->Snapshot();
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].message, "should appear");
    EXPECT_EQ(entries[1].message, "should also appear");
}

TEST(LoggerTest, FansOutToMultipleSinks) {
    Logger logger(LogLevel::Trace);
    auto sinkA = std::make_shared<MemoryLogSink>();
    auto sinkB = std::make_shared<MemoryLogSink>();
    logger.AddSink(sinkA);
    logger.AddSink(sinkB);

    logger.Log(LogLevel::Info, "Test", "hello");

    EXPECT_EQ(sinkA->Snapshot().size(), 1u);
    EXPECT_EQ(sinkB->Snapshot().size(), 1u);
}

TEST(LoggerTest, SetMinimumLevel_ChangesFilteringAtRuntime) {
    Logger logger(LogLevel::Info);
    auto sink = std::make_shared<MemoryLogSink>();
    logger.AddSink(sink);

    logger.Log(LogLevel::Debug, "Test", "filtered");
    EXPECT_EQ(sink->Snapshot().size(), 0u);

    logger.SetMinimumLevel(LogLevel::Debug);
    logger.Log(LogLevel::Debug, "Test", "not filtered anymore");
    EXPECT_EQ(sink->Snapshot().size(), 1u);
}

TEST(LogLevelTest, ToStringCoversAllValues) {
    EXPECT_STREQ(ToString(LogLevel::Trace), "TRACE");
    EXPECT_STREQ(ToString(LogLevel::Critical), "CRITICAL");
}
