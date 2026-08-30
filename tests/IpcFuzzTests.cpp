// Stage 2.5 hardening: PRIORITY 8 (IPC) + PRIORITY 10 (fuzzing ->
// permanent regression tests). Converts manual fuzzing performed during
// Stage 2.5 hardening into permanent, reproducible, seeded regression
// tests, and adds explicit coverage for every scenario the audit's IPC
// checklist calls out: wrong/future/old protocol version, unknown
// message type, truncated message, oversized message, invalid lengths,
// malformed payloads. (Disconnect-during-request/response and
// shutdown-while-client-connected scenarios require the real
// Win32NamedPipeTransport and cannot be exercised from this Linux
// sandbox — see Win32NamedPipeSecurityTests.cpp and
// CacheEngineShutdownRaceTests.cpp's IPC-adjacent coverage for what
// COULD be verified, and docs/ENVIRONMENT.md for what remains
// real-Windows-only.)
//
// The core safety property under test throughout: NO malformed input,
// no matter how corrupted or adversarial, may crash (segfault, throw an
// uncaught exception, trigger UB) or hang the decoder. Every malformed
// input must produce a clean Result<T>::Failure(...).
#include "QuantumCache/Ipc/MessageCodec.h"
#include <gtest/gtest.h>
#include <cstring>
#include <random>

using namespace QuantumCache;
namespace Ipc = QuantumCache::Ipc;

namespace {
template <typename T>
void AppendRaw(std::vector<std::uint8_t>& buf, const T& v) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&v);
    buf.insert(buf.end(), bytes, bytes + sizeof(T));
}

// Hand-crafts a raw frame with an explicit header (length, type,
// version) so tests can construct exactly the malformed/edge-case
// frames the audit's checklist calls for, independent of MessageCodec's
// own (correct) encoders.
std::vector<std::uint8_t> MakeRawFrame(std::uint32_t type, std::uint32_t version,
                                        const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> frame;
    AppendRaw(frame, static_cast<std::uint32_t>(0)); // length placeholder
    AppendRaw(frame, type);
    AppendRaw(frame, version);
    frame.insert(frame.end(), payload.begin(), payload.end());
    std::uint32_t totalLength = static_cast<std::uint32_t>(frame.size());
    std::memcpy(frame.data(), &totalLength, sizeof(totalLength));
    return frame;
}
} // namespace

// ---------------------------------------------------------------------
// Protocol version edge cases.
// ---------------------------------------------------------------------

TEST(IpcFuzzTest, FutureProtocolVersion_IsRejected_NotCrashed) {
    // A hypothetical future client/server speaking a NEWER protocol
    // version than this build understands.
    auto frame = MakeRawFrame(static_cast<std::uint32_t>(Ipc::MessageType::GetStatusResponse),
                               Ipc::kProtocolVersion + 1, {});
    auto result = Ipc::MessageCodec::DecodeStatusResponse(frame);
    EXPECT_FALSE(result.IsOk());
    EXPECT_EQ(result.Err().code, Common::ErrorCode::VersionMismatch);
}

TEST(IpcFuzzTest, OldProtocolVersion_IsRejected_NotCrashed) {
    auto frame = MakeRawFrame(static_cast<std::uint32_t>(Ipc::MessageType::GetStatusResponse),
                               Ipc::kProtocolVersion > 0 ? Ipc::kProtocolVersion - 1 : 0, {});
    auto result = Ipc::MessageCodec::DecodeStatusResponse(frame);
    EXPECT_FALSE(result.IsOk());
    EXPECT_EQ(result.Err().code, Common::ErrorCode::VersionMismatch);
}

TEST(IpcFuzzTest, ZeroProtocolVersion_IsRejected_NotCrashed) {
    auto frame = MakeRawFrame(static_cast<std::uint32_t>(Ipc::MessageType::GetStatusResponse), 0, {});
    auto result = Ipc::MessageCodec::DecodeStatusResponse(frame);
    EXPECT_FALSE(result.IsOk());
    EXPECT_EQ(result.Err().code, Common::ErrorCode::VersionMismatch);
}

TEST(IpcFuzzTest, MaxUint32ProtocolVersion_IsRejected_NotCrashed) {
    auto frame = MakeRawFrame(static_cast<std::uint32_t>(Ipc::MessageType::GetStatusResponse),
                               0xFFFFFFFFu, {});
    auto result = Ipc::MessageCodec::DecodeStatusResponse(frame);
    EXPECT_FALSE(result.IsOk());
    EXPECT_EQ(result.Err().code, Common::ErrorCode::VersionMismatch);
}

// ---------------------------------------------------------------------
// Message type edge cases.
// ---------------------------------------------------------------------

TEST(IpcFuzzTest, UnknownMessageType_IsRejected_NotCrashed) {
    // A message type value that does not correspond to any defined
    // MessageType enumerator at all (not merely "the wrong one for this
    // decoder" -- genuinely unknown/未来/invalid).
    auto frame = MakeRawFrame(0xDEADBEEFu, Ipc::kProtocolVersion, {});
    auto result = Ipc::MessageCodec::DecodeStatusResponse(frame);
    EXPECT_FALSE(result.IsOk());
    // Either InvalidArgument ("not a GetStatusResponse frame") or
    // CorruptData is an acceptable, safe outcome -- the property under
    // test is "rejected cleanly, never crashes," not a specific code.
}

TEST(IpcFuzzTest, PeekMessageType_UnknownType_StillReturnsSuccessfully) {
    // PeekMessageType() deliberately does not validate the type is
    // known (it exists so callers can dispatch BEFORE picking the right
    // decoder) -- confirm it still handles an unknown type value
    // without crashing, returning the raw value for the caller's own
    // switch/dispatch to reject.
    auto frame = MakeRawFrame(0xDEADBEEFu, Ipc::kProtocolVersion, {});
    auto result = Ipc::MessageCodec::PeekMessageType(frame);
    ASSERT_TRUE(result.IsOk());
    EXPECT_EQ(static_cast<std::uint32_t>(result.Value()), 0xDEADBEEFu);
}

TEST(IpcFuzzTest, ZeroMessageType_IsRejected_NotCrashed) {
    // Type 0 is not assigned to any MessageType enumerator (they start
    // at 1) -- a plausible "all-zero garbage frame" pattern.
    auto frame = MakeRawFrame(0u, Ipc::kProtocolVersion, {});
    auto result = Ipc::MessageCodec::DecodeStatusResponse(frame);
    EXPECT_FALSE(result.IsOk());
}

// ---------------------------------------------------------------------
// Truncated / oversized / malformed-length messages.
// ---------------------------------------------------------------------

TEST(IpcFuzzTest, EmptyFrame_IsRejected_NotCrashed) {
    std::vector<std::uint8_t> empty;
    auto r1 = Ipc::MessageCodec::DecodeStatusResponse(empty);
    auto r2 = Ipc::MessageCodec::DecodeCacheStatisticsResponse(empty);
    auto r3 = Ipc::MessageCodec::DecodeInvalidateKeyRequest(empty);
    auto r4 = Ipc::MessageCodec::PeekMessageType(empty);
    EXPECT_FALSE(r1.IsOk());
    EXPECT_FALSE(r2.IsOk());
    EXPECT_FALSE(r3.IsOk());
    EXPECT_FALSE(r4.IsOk());
}

TEST(IpcFuzzTest, OneByteFrame_IsRejected_NotCrashed) {
    std::vector<std::uint8_t> tiny = {0x42};
    EXPECT_FALSE(Ipc::MessageCodec::DecodeStatusResponse(tiny).IsOk());
    EXPECT_FALSE(Ipc::MessageCodec::PeekMessageType(tiny).IsOk());
}

TEST(IpcFuzzTest, HeaderOnly_NoPayload_TruncatedFrame_IsRejected_NotCrashed) {
    // A well-formed 12-byte header (length, type, version) claiming to
    // be a GetCacheStatisticsResponse, but with NONE of the required
    // payload fields present.
    auto frame = MakeRawFrame(static_cast<std::uint32_t>(Ipc::MessageType::GetCacheStatisticsResponse),
                               Ipc::kProtocolVersion, {});
    auto result = Ipc::MessageCodec::DecodeCacheStatisticsResponse(frame);
    EXPECT_FALSE(result.IsOk());
    EXPECT_EQ(result.Err().code, Common::ErrorCode::CorruptData);
}

TEST(IpcFuzzTest, LengthFieldLiesAboutFrameSize_TooLarge_IsRejected_NotCrashed) {
    // The length field claims the frame is much larger than the actual
    // number of bytes provided -- must be detected via the
    // "totalLength != frame.size()" check, never trusted to index past
    // the real buffer.
    auto frame = MakeRawFrame(static_cast<std::uint32_t>(Ipc::MessageType::GetStatusResponse),
                               Ipc::kProtocolVersion, {});
    // Corrupt the length field to claim the frame is 10x larger than it
    // actually is.
    std::uint32_t fakeLength = static_cast<std::uint32_t>(frame.size()) * 10;
    std::memcpy(frame.data(), &fakeLength, sizeof(fakeLength));
    auto result = Ipc::MessageCodec::DecodeStatusResponse(frame);
    EXPECT_FALSE(result.IsOk());
    EXPECT_EQ(result.Err().code, Common::ErrorCode::CorruptData);
}

TEST(IpcFuzzTest, LengthFieldLiesAboutFrameSize_TooSmall_IsRejected_NotCrashed) {
    auto frame = MakeRawFrame(static_cast<std::uint32_t>(Ipc::MessageType::GetStatusResponse),
                               Ipc::kProtocolVersion, {});
    std::uint32_t fakeLength = 4; // absurdly small
    std::memcpy(frame.data(), &fakeLength, sizeof(fakeLength));
    auto result = Ipc::MessageCodec::DecodeStatusResponse(frame);
    EXPECT_FALSE(result.IsOk());
}

TEST(IpcFuzzTest, InvalidateKeyRequest_StringLengthField_ClaimsMoreThanAvailable_IsRejected) {
    // The InvalidateKeyRequest payload has a length-prefixed string
    // (key). Craft a frame whose string-length field claims far more
    // bytes than are actually present in the frame.
    std::vector<std::uint8_t> payload;
    std::uint32_t fakeStringLength = 0xFFFFFF00u; // absurd, ~4GB
    AppendRaw(payload, fakeStringLength);
    payload.insert(payload.end(), {'a', 'b', 'c'}); // far fewer bytes than claimed

    auto frame = MakeRawFrame(static_cast<std::uint32_t>(Ipc::MessageType::InvalidateKeyRequest),
                               Ipc::kProtocolVersion, payload);
    auto result = Ipc::MessageCodec::DecodeInvalidateKeyRequest(frame);
    EXPECT_FALSE(result.IsOk())
        << "a string-length field claiming far more bytes than the frame actually contains "
           "must be rejected, never used to read out-of-bounds memory";
}

TEST(IpcFuzzTest, InvalidateKeyRequest_StringLengthField_ExceedsSaneMax_IsRejected) {
    // Even if (hypothetically) the frame WERE that large, an absurd
    // string length must still be rejected by the codec's own sanity
    // bound (kMaxReasonableLength in MessageCodec.cpp) rather than
    // attempting a multi-gigabyte allocation.
    std::vector<std::uint8_t> payload;
    std::uint32_t justOverSaneMax = 16u * 1024u * 1024u + 1u; // kMaxReasonableLength + 1
    AppendRaw(payload, justOverSaneMax);
    // Pad with a modest amount of real data (irrelevant, since the
    // length check must fail before this is ever read).
    payload.insert(payload.end(), 100, 'x');

    auto frame = MakeRawFrame(static_cast<std::uint32_t>(Ipc::MessageType::InvalidateKeyRequest),
                               Ipc::kProtocolVersion, payload);
    auto result = Ipc::MessageCodec::DecodeInvalidateKeyRequest(frame);
    EXPECT_FALSE(result.IsOk());
}

TEST(IpcFuzzTest, OperationResult_MessageLengthField_ClaimsMoreThanAvailable_IsRejected) {
    std::vector<std::uint8_t> payload;
    std::uint8_t succeeded = 1;
    std::uint32_t errorCode = 0;
    AppendRaw(payload, succeeded);
    AppendRaw(payload, errorCode);
    std::uint32_t fakeMessageLength = 0x7FFFFFFFu;
    AppendRaw(payload, fakeMessageLength);
    // no actual message bytes follow

    auto frame = MakeRawFrame(static_cast<std::uint32_t>(Ipc::MessageType::FlushAllResponse),
                               Ipc::kProtocolVersion, payload);
    auto result = Ipc::MessageCodec::DecodeOperationResult(frame, Ipc::MessageType::FlushAllResponse);
    EXPECT_FALSE(result.IsOk());
}

TEST(IpcFuzzTest, OversizedButWellFormedFrame_LargeValidKey_Succeeds) {
    // A LEGITIMATELY large (but within the sane bound) InvalidateKeyRequest
    // must still succeed -- the sane-length bound must not be so
    // aggressive it rejects real, valid large-but-reasonable input.
    std::string bigKey(1024 * 1024, 'k'); // 1 MiB key, well under the 16 MiB cap
    Ipc::InvalidateKeyRequestPayload payload;
    payload.key = bigKey;
    auto encoded = Ipc::MessageCodec::EncodeInvalidateKeyRequest(payload);
    auto decoded = Ipc::MessageCodec::DecodeInvalidateKeyRequest(encoded);
    ASSERT_TRUE(decoded.IsOk());
    EXPECT_EQ(decoded.Value().key, bigKey);
}

// ---------------------------------------------------------------------
// Cross-decoder confusion: feeding one message type's frame into a
// DIFFERENT message type's decoder (a realistic "wrong handler called"
// bug class, or an adversarial client deliberately sending a
// type-mismatched frame).
// ---------------------------------------------------------------------

TEST(IpcFuzzTest, CrossDecoderConfusion_StatusResponseFrame_IntoInvalidateKeyDecoder_IsRejected) {
    Ipc::StatusResponsePayload statusPayload;
    statusPayload.recoveryState = 1;
    statusPayload.sessionGeneration = 42;
    statusPayload.cacheEngineActive = true;
    auto statusFrame = Ipc::MessageCodec::EncodeStatusResponse(statusPayload);

    auto result = Ipc::MessageCodec::DecodeInvalidateKeyRequest(statusFrame);
    EXPECT_FALSE(result.IsOk())
        << "a StatusResponse frame fed into the InvalidateKeyRequest decoder must be rejected "
           "via the type-mismatch check, never partially/incorrectly parsed";
}

TEST(IpcFuzzTest, CrossDecoderConfusion_CacheStatsFrame_IntoOperationResultDecoder_IsRejected) {
    Ipc::CacheStatisticsPayload statsPayload{};
    auto statsFrame = Ipc::MessageCodec::EncodeCacheStatisticsResponse(statsPayload);
    auto result = Ipc::MessageCodec::DecodeOperationResult(statsFrame, Ipc::MessageType::FlushAllResponse);
    EXPECT_FALSE(result.IsOk());
}

// ---------------------------------------------------------------------
// Seeded, reproducible fuzz tests (Priority 10: manual fuzzing ->
// permanent automated regression tests, with recorded seeds).
// ---------------------------------------------------------------------

TEST(IpcFuzzTest, SeededRandomFuzz_Seed12345_AllDecodersNeverCrash) {
    // Fixed seed for reproducibility -- if this ever finds a crash in
    // the future, the exact same sequence can be replayed deterministically.
    std::mt19937 rng(12345);
    constexpr int kIterations = 20000;
    for (int iter = 0; iter < kIterations; ++iter) {
        std::vector<std::uint8_t> garbage;
        int len = static_cast<int>(rng() % 512);
        garbage.reserve(static_cast<std::size_t>(len));
        for (int i = 0; i < len; ++i) {
            garbage.push_back(static_cast<std::uint8_t>(rng() % 256));
        }
        // Every public decoder must handle this without crashing.
        (void)Ipc::MessageCodec::DecodeStatusResponse(garbage);
        (void)Ipc::MessageCodec::DecodeCacheStatisticsResponse(garbage);
        (void)Ipc::MessageCodec::DecodeOperationResult(garbage, Ipc::MessageType::FlushAllResponse);
        (void)Ipc::MessageCodec::DecodeOperationResult(garbage, Ipc::MessageType::InvalidateKeyResponse);
        (void)Ipc::MessageCodec::DecodeInvalidateKeyRequest(garbage);
        (void)Ipc::MessageCodec::PeekMessageType(garbage);
    }
    SUCCEED() << "completed " << kIterations << " random-buffer fuzz iterations (seed=12345) "
                 "against all 6 public decoder entry points with no crash/hang/UB";
}

TEST(IpcFuzzTest, SeededRandomFuzz_Seed999_StructuredNearValidFrames_NeverCrash) {
    // A second, differently-seeded fuzz pass that biases toward
    // STRUCTURALLY-plausible frames (correct-looking header, random
    // payload) rather than fully random bytes -- more likely to
    // exercise the payload-parsing logic specifically, since fully
    // random headers are rejected almost immediately by the version/
    // type/length checks.
    std::mt19937 rng(999);
    constexpr int kIterations = 20000;
    for (int iter = 0; iter < kIterations; ++iter) {
        std::uint32_t type = rng() % 10; // spans real + a few invalid type values
        std::uint32_t version = (rng() % 4 == 0) ? Ipc::kProtocolVersion : (rng() % 5); // sometimes correct
        std::vector<std::uint8_t> payload;
        int payloadLen = static_cast<int>(rng() % 256);
        for (int i = 0; i < payloadLen; ++i) {
            payload.push_back(static_cast<std::uint8_t>(rng() % 256));
        }
        auto frame = MakeRawFrame(type, version, payload);

        (void)Ipc::MessageCodec::DecodeStatusResponse(frame);
        (void)Ipc::MessageCodec::DecodeCacheStatisticsResponse(frame);
        (void)Ipc::MessageCodec::DecodeOperationResult(frame, Ipc::MessageType::FlushAllResponse);
        (void)Ipc::MessageCodec::DecodeInvalidateKeyRequest(frame);
        (void)Ipc::MessageCodec::PeekMessageType(frame);
    }
    SUCCEED() << "completed " << kIterations << " structured-near-valid-frame fuzz iterations "
                 "(seed=999) with no crash/hang/UB";
}

TEST(IpcFuzzTest, SeededRandomFuzz_Seed777_TruncationAtEveryPossibleLength_NeverCrash) {
    // Specifically targets truncation: take a REAL, well-formed encoded
    // frame for each message type and truncate it to every possible
    // length from 0 to its full size, feeding each truncated prefix
    // into the matching decoder. This exhaustively covers "the frame
    // was cut off mid-transmission" (a real, plausible IPC failure mode
    // -- a pipe read returning fewer bytes than expected, or a
    // disconnect mid-message) at every single byte boundary, not just
    // random ones.
    Ipc::StatusResponsePayload statusPayload;
    statusPayload.recoveryState = 2;
    statusPayload.sessionGeneration = 7;
    statusPayload.cacheEngineActive = false;
    auto statusFrame = Ipc::MessageCodec::EncodeStatusResponse(statusPayload);
    for (std::size_t len = 0; len <= statusFrame.size(); ++len) {
        std::vector<std::uint8_t> truncated(statusFrame.begin(), statusFrame.begin() + static_cast<long>(len));
        (void)Ipc::MessageCodec::DecodeStatusResponse(truncated);
    }

    Ipc::InvalidateKeyRequestPayload invalidatePayload;
    invalidatePayload.key = "a-reasonably-long-test-key-1234567890";
    auto invalidateFrame = Ipc::MessageCodec::EncodeInvalidateKeyRequest(invalidatePayload);
    for (std::size_t len = 0; len <= invalidateFrame.size(); ++len) {
        std::vector<std::uint8_t> truncated(invalidateFrame.begin(), invalidateFrame.begin() + static_cast<long>(len));
        (void)Ipc::MessageCodec::DecodeInvalidateKeyRequest(truncated);
    }

    Ipc::CacheStatisticsPayload statsPayload{};
    statsPayload.hitCount = 100;
    statsPayload.missCount = 5;
    auto statsFrame = Ipc::MessageCodec::EncodeCacheStatisticsResponse(statsPayload);
    for (std::size_t len = 0; len <= statsFrame.size(); ++len) {
        std::vector<std::uint8_t> truncated(statsFrame.begin(), statsFrame.begin() + static_cast<long>(len));
        (void)Ipc::MessageCodec::DecodeCacheStatisticsResponse(truncated);
    }

    SUCCEED() << "exhaustively truncated 3 real encoded frames at every possible byte length "
                 "with no crash/hang/UB in any decoder";
}
