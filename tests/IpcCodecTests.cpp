#include "QuantumCache/Ipc/MessageCodec.h"
#include <gtest/gtest.h>
#include <cstring>

using namespace QuantumCache::Ipc;

TEST(MessageCodecTest, StatusResponse_RoundTrips) {
    StatusResponsePayload payload;
    payload.recoveryState = 4; // RecoveryComplete numeric value
    payload.sessionGeneration = 12345;
    payload.cacheEngineActive = false;

    auto frame = MessageCodec::EncodeStatusResponse(payload);
    auto decoded = MessageCodec::DecodeStatusResponse(frame);

    ASSERT_TRUE(decoded.IsOk());
    EXPECT_EQ(decoded.Value().recoveryState, 4u);
    EXPECT_EQ(decoded.Value().sessionGeneration, 12345u);
    EXPECT_FALSE(decoded.Value().cacheEngineActive);
}

TEST(MessageCodecTest, DecodeStatusResponse_RejectsWrongMessageType) {
    auto emptyFrame = MessageCodec::EncodeEmpty(MessageType::GetStatusRequest);
    auto decoded = MessageCodec::DecodeStatusResponse(emptyFrame);
    EXPECT_FALSE(decoded.IsOk());
}

TEST(MessageCodecTest, DecodeStatusResponse_RejectsTruncatedFrame) {
    std::vector<std::uint8_t> truncated = {1, 2, 3};
    auto decoded = MessageCodec::DecodeStatusResponse(truncated);
    EXPECT_FALSE(decoded.IsOk());
}

TEST(MessageCodecTest, PeekMessageType_ReadsTypeWithoutFullDecode) {
    auto frame = MessageCodec::EncodeEmpty(MessageType::GetStatusRequest);
    auto type = MessageCodec::PeekMessageType(frame);
    ASSERT_TRUE(type.IsOk());
    EXPECT_EQ(type.Value(), MessageType::GetStatusRequest);
}

TEST(MessageCodecTest, EncodeEmpty_ProducesCorrectLengthPrefix) {
    auto frame = MessageCodec::EncodeEmpty(MessageType::GetStatusRequest);
    std::uint32_t length;
    std::memcpy(&length, frame.data(), sizeof(length));
    EXPECT_EQ(length, frame.size());
}

// ---------------------------------------------------------------------
// Stage 2: cache management messages.
// ---------------------------------------------------------------------

TEST(MessageCodecTest, CacheStatistics_RoundTrips) {
    CacheStatisticsPayload payload;
    payload.hitCount = 100;
    payload.missCount = 20;
    payload.insertCount = 15;
    payload.updateCount = 5;
    payload.invalidationCount = 3;
    payload.evictionCount = 7;
    payload.flushSuccessCount = 12;
    payload.flushFailureCount = 1;
    payload.currentEntryCount = 42;
    payload.currentMemoryBytes = 123456;
    payload.dirtyEntryCount = 4;
    payload.dirtyBytes = 4096;

    auto frame = MessageCodec::EncodeCacheStatisticsResponse(payload);
    auto decoded = MessageCodec::DecodeCacheStatisticsResponse(frame);

    ASSERT_TRUE(decoded.IsOk());
    EXPECT_EQ(decoded.Value().hitCount, 100u);
    EXPECT_EQ(decoded.Value().missCount, 20u);
    EXPECT_EQ(decoded.Value().insertCount, 15u);
    EXPECT_EQ(decoded.Value().updateCount, 5u);
    EXPECT_EQ(decoded.Value().invalidationCount, 3u);
    EXPECT_EQ(decoded.Value().evictionCount, 7u);
    EXPECT_EQ(decoded.Value().flushSuccessCount, 12u);
    EXPECT_EQ(decoded.Value().flushFailureCount, 1u);
    EXPECT_EQ(decoded.Value().currentEntryCount, 42u);
    EXPECT_EQ(decoded.Value().currentMemoryBytes, 123456u);
    EXPECT_EQ(decoded.Value().dirtyEntryCount, 4u);
    EXPECT_EQ(decoded.Value().dirtyBytes, 4096u);
}

TEST(MessageCodecTest, CacheStatistics_RejectsWrongMessageType) {
    auto frame = MessageCodec::EncodeEmpty(MessageType::GetStatusRequest);
    auto decoded = MessageCodec::DecodeCacheStatisticsResponse(frame);
    EXPECT_FALSE(decoded.IsOk());
}

TEST(MessageCodecTest, OperationResult_SuccessRoundTrips) {
    OperationResultPayload payload;
    payload.succeeded = true;
    payload.errorCode = 0;
    payload.message = "flushed 12 entries";

    auto frame = MessageCodec::EncodeOperationResult(MessageType::FlushAllResponse, payload);
    auto decoded = MessageCodec::DecodeOperationResult(frame, MessageType::FlushAllResponse);

    ASSERT_TRUE(decoded.IsOk());
    EXPECT_TRUE(decoded.Value().succeeded);
    EXPECT_EQ(decoded.Value().errorCode, 0u);
    EXPECT_EQ(decoded.Value().message, "flushed 12 entries");
}

TEST(MessageCodecTest, OperationResult_FailureRoundTrips) {
    OperationResultPayload payload;
    payload.succeeded = false;
    payload.errorCode = 404;
    payload.message = "key not found";

    auto frame = MessageCodec::EncodeOperationResult(MessageType::InvalidateKeyResponse, payload);
    auto decoded = MessageCodec::DecodeOperationResult(frame, MessageType::InvalidateKeyResponse);

    ASSERT_TRUE(decoded.IsOk());
    EXPECT_FALSE(decoded.Value().succeeded);
    EXPECT_EQ(decoded.Value().errorCode, 404u);
    EXPECT_EQ(decoded.Value().message, "key not found");
}

TEST(MessageCodecTest, OperationResult_RejectsMismatchedExpectedType) {
    OperationResultPayload payload;
    payload.succeeded = true;
    auto frame = MessageCodec::EncodeOperationResult(MessageType::FlushAllResponse, payload);
    // Decode expecting a DIFFERENT type than what was encoded.
    auto decoded = MessageCodec::DecodeOperationResult(frame, MessageType::InvalidateKeyResponse);
    EXPECT_FALSE(decoded.IsOk());
}

TEST(MessageCodecTest, InvalidateKeyRequest_RoundTrips) {
    InvalidateKeyRequestPayload payload;
    payload.key = "some/cache/key";

    auto frame = MessageCodec::EncodeInvalidateKeyRequest(payload);
    auto decoded = MessageCodec::DecodeInvalidateKeyRequest(frame);

    ASSERT_TRUE(decoded.IsOk());
    EXPECT_EQ(decoded.Value().key, "some/cache/key");
}

TEST(MessageCodecTest, InvalidateKeyRequest_EmptyKeyRoundTrips) {
    InvalidateKeyRequestPayload payload;
    payload.key = "";
    auto frame = MessageCodec::EncodeInvalidateKeyRequest(payload);
    auto decoded = MessageCodec::DecodeInvalidateKeyRequest(frame);
    ASSERT_TRUE(decoded.IsOk());
    EXPECT_EQ(decoded.Value().key, "");
}

TEST(MessageCodecTest, InvalidateKeyRequest_RejectsTruncatedFrame) {
    std::vector<std::uint8_t> truncated = {1, 2, 3, 4};
    auto decoded = MessageCodec::DecodeInvalidateKeyRequest(truncated);
    EXPECT_FALSE(decoded.IsOk());
}

TEST(MessageCodecTest, InvalidateKeyRequest_RejectsOversizedKeyLength) {
    std::vector<std::uint8_t> frame;
    // Length placeholder (patched later), MessageType::InvalidateKeyRequest, kProtocolVersion
    std::uint32_t totalLen = 16;
    std::uint32_t msgType = static_cast<std::uint32_t>(MessageType::InvalidateKeyRequest);
    std::uint32_t ver = kProtocolVersion;
    std::uint32_t keyLen = 100u * 1024u; // 100 KiB, exceeds default 64 KiB bound

    const auto* p1 = reinterpret_cast<const std::uint8_t*>(&totalLen);
    frame.insert(frame.end(), p1, p1 + 4);
    const auto* p2 = reinterpret_cast<const std::uint8_t*>(&msgType);
    frame.insert(frame.end(), p2, p2 + 4);
    const auto* p3 = reinterpret_cast<const std::uint8_t*>(&ver);
    frame.insert(frame.end(), p3, p3 + 4);
    const auto* p4 = reinterpret_cast<const std::uint8_t*>(&keyLen);
    frame.insert(frame.end(), p4, p4 + 4);

    auto decoded = MessageCodec::DecodeInvalidateKeyRequest(frame);
    EXPECT_FALSE(decoded.IsOk());
    if (!decoded.IsOk()) {
        EXPECT_EQ(decoded.Err().code, QuantumCache::Common::ErrorCode::CorruptData);
    }
}

TEST(MessageCodecTest, ProtocolVersionMismatch_IsDetected) {
    // Manually craft a frame with a stale protocol version to confirm the
    // decoder actually checks it rather than accepting anything.
    auto frame = MessageCodec::EncodeEmpty(MessageType::GetStatusRequest);
    std::uint32_t staleVersion = kProtocolVersion + 1;
    std::memcpy(frame.data() + 8, &staleVersion, sizeof(staleVersion));

    auto peeked = MessageCodec::PeekMessageType(frame);
    ASSERT_TRUE(peeked.IsOk()); // PeekMessageType does not check version by design

    OperationResultPayload dummy;
    auto asOpResult = MessageCodec::DecodeOperationResult(frame, MessageType::GetStatusRequest);
    EXPECT_FALSE(asOpResult.IsOk());
    if (!asOpResult.IsOk()) {
        EXPECT_EQ(asOpResult.Err().code, QuantumCache::Common::ErrorCode::VersionMismatch);
    }
}
