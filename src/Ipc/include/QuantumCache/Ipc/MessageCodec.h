#pragma once
#include "QuantumCache/Common/Result.h"
#include "QuantumCache/Ipc/Protocol.h"
#include <cstdint>
#include <vector>

namespace QuantumCache::Ipc {

// Real (not stubbed) binary encode/decode for the Stage 1 IPC messages.
// Kept separate from the transport (INamedPipeTransport) so the wire
// format can be unit tested on this Linux sandbox without any named pipe
// / Win32 dependency.
class MessageCodec {
public:
    [[nodiscard]] static std::vector<std::uint8_t> EncodeStatusResponse(
        const StatusResponsePayload& payload);

    [[nodiscard]] static Common::Result<StatusResponsePayload> DecodeStatusResponse(
        const std::vector<std::uint8_t>& frame);

    // Encodes just the frame header (type + version) for simple,
    // payload-less messages such as GetStatusRequest.
    [[nodiscard]] static std::vector<std::uint8_t> EncodeEmpty(MessageType type);

    [[nodiscard]] static Common::Result<MessageType> PeekMessageType(
        const std::vector<std::uint8_t>& frame);

    // --- Stage 2: cache management messages ---

    [[nodiscard]] static std::vector<std::uint8_t> EncodeCacheStatisticsResponse(
        const CacheStatisticsPayload& payload);
    [[nodiscard]] static Common::Result<CacheStatisticsPayload> DecodeCacheStatisticsResponse(
        const std::vector<std::uint8_t>& frame);

    [[nodiscard]] static std::vector<std::uint8_t> EncodeOperationResult(
        MessageType type, const OperationResultPayload& payload);
    [[nodiscard]] static Common::Result<OperationResultPayload> DecodeOperationResult(
        const std::vector<std::uint8_t>& frame, MessageType expectedType);

    [[nodiscard]] static std::vector<std::uint8_t> EncodeInvalidateKeyRequest(
        const InvalidateKeyRequestPayload& payload);
    [[nodiscard]] static Common::Result<InvalidateKeyRequestPayload> DecodeInvalidateKeyRequest(
        const std::vector<std::uint8_t>& frame);
};

} // namespace QuantumCache::Ipc
