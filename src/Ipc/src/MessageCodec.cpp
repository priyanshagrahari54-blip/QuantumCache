#include "QuantumCache/Ipc/MessageCodec.h"
#include <cstring>

namespace QuantumCache::Ipc {
namespace {

using Common::Error;
using Common::ErrorCode;
using Common::Result;

constexpr std::size_t kHeaderSize = sizeof(std::uint32_t) * 3; // length, type, version

template <typename T>
void AppendRaw(std::vector<std::uint8_t>& buffer, const T& value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    buffer.insert(buffer.end(), bytes, bytes + sizeof(T));
}

template <typename T>
bool ReadRaw(const std::vector<std::uint8_t>& buffer, std::size_t& offset, T& out) {
    if (offset + sizeof(T) > buffer.size()) return false;
    std::memcpy(&out, buffer.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

} // namespace

std::vector<std::uint8_t> MessageCodec::EncodeStatusResponse(const StatusResponsePayload& payload) {
    std::vector<std::uint8_t> frame;
    frame.reserve(kHeaderSize + sizeof(StatusResponsePayload));

    // Reserve space for length, filled in at the end.
    AppendRaw(frame, static_cast<std::uint32_t>(0));
    AppendRaw(frame, static_cast<std::uint32_t>(MessageType::GetStatusResponse));
    AppendRaw(frame, kProtocolVersion);

    AppendRaw(frame, payload.recoveryState);
    AppendRaw(frame, payload.sessionGeneration);
    std::uint8_t activeByte = payload.cacheEngineActive ? 1 : 0;
    AppendRaw(frame, activeByte);

    std::uint32_t totalLength = static_cast<std::uint32_t>(frame.size());
    std::memcpy(frame.data(), &totalLength, sizeof(totalLength));
    return frame;
}

Result<StatusResponsePayload> MessageCodec::DecodeStatusResponse(const std::vector<std::uint8_t>& frame) {
    std::size_t offset = 0;
    std::uint32_t totalLength = 0, typeRaw = 0, version = 0;

    if (!ReadRaw(frame, offset, totalLength) ||
        !ReadRaw(frame, offset, typeRaw) ||
        !ReadRaw(frame, offset, version)) {
        return Result<StatusResponsePayload>::Failure(
            Error{ErrorCode::CorruptData, "IPC frame too short for header", 0});
    }

    if (totalLength != frame.size()) {
        return Result<StatusResponsePayload>::Failure(
            Error{ErrorCode::CorruptData, "IPC frame length mismatch", 0});
    }
    if (typeRaw != static_cast<std::uint32_t>(MessageType::GetStatusResponse)) {
        return Result<StatusResponsePayload>::Failure(
            Error{ErrorCode::InvalidArgument, "not a GetStatusResponse frame", 0});
    }
    if (version != kProtocolVersion) {
        return Result<StatusResponsePayload>::Failure(
            Error{ErrorCode::VersionMismatch, "unsupported IPC protocol version", 0});
    }

    StatusResponsePayload payload;
    std::uint8_t activeByte = 0;
    if (!ReadRaw(frame, offset, payload.recoveryState) ||
        !ReadRaw(frame, offset, payload.sessionGeneration) ||
        !ReadRaw(frame, offset, activeByte)) {
        return Result<StatusResponsePayload>::Failure(
            Error{ErrorCode::CorruptData, "IPC frame truncated payload", 0});
    }
    payload.cacheEngineActive = (activeByte != 0);

    return Result<StatusResponsePayload>::Success(payload);
}

std::vector<std::uint8_t> MessageCodec::EncodeEmpty(MessageType type) {
    std::vector<std::uint8_t> frame;
    AppendRaw(frame, static_cast<std::uint32_t>(0));
    AppendRaw(frame, static_cast<std::uint32_t>(type));
    AppendRaw(frame, kProtocolVersion);
    std::uint32_t totalLength = static_cast<std::uint32_t>(frame.size());
    std::memcpy(frame.data(), &totalLength, sizeof(totalLength));
    return frame;
}

Result<MessageType> MessageCodec::PeekMessageType(const std::vector<std::uint8_t>& frame) {
    std::size_t offset = 0;
    std::uint32_t totalLength = 0, typeRaw = 0;
    if (!ReadRaw(frame, offset, totalLength) || !ReadRaw(frame, offset, typeRaw)) {
        return Result<MessageType>::Failure(
            Error{ErrorCode::CorruptData, "IPC frame too short to contain a type", 0});
    }
    return Result<MessageType>::Success(static_cast<MessageType>(typeRaw));
}

namespace {

void AppendHeaderPlaceholder(std::vector<std::uint8_t>& frame, MessageType type) {
    AppendRaw(frame, static_cast<std::uint32_t>(0)); // length placeholder, patched below
    AppendRaw(frame, static_cast<std::uint32_t>(type));
    AppendRaw(frame, kProtocolVersion);
}

void PatchLength(std::vector<std::uint8_t>& frame) {
    std::uint32_t totalLength = static_cast<std::uint32_t>(frame.size());
    std::memcpy(frame.data(), &totalLength, sizeof(totalLength));
}

void AppendString(std::vector<std::uint8_t>& frame, const std::string& s) {
    AppendRaw(frame, static_cast<std::uint32_t>(s.size()));
    frame.insert(frame.end(), s.begin(), s.end());
}

bool ReadString(const std::vector<std::uint8_t>& frame, std::size_t& offset, std::string& out,
                std::uint32_t maxAllowedLength = 64u * 1024u) {
    std::uint32_t length = 0;
    if (!ReadRaw(frame, offset, length)) return false;
    // Sanity bound to prevent memory exhaustion / DoS attacks from untrusted IPC input.
    if (length > maxAllowedLength || offset + length > frame.size()) return false;
    out.assign(frame.begin() + static_cast<long>(offset), frame.begin() + static_cast<long>(offset + length));
    offset += length;
    return true;
}

Result<void> CheckHeader(const std::vector<std::uint8_t>& frame, std::size_t& offset,
                          MessageType expectedType) {
    std::uint32_t totalLength = 0, typeRaw = 0, version = 0;
    if (!ReadRaw(frame, offset, totalLength) || !ReadRaw(frame, offset, typeRaw) ||
        !ReadRaw(frame, offset, version)) {
        return Result<void>::Failure(Error{ErrorCode::CorruptData, "IPC frame too short for header", 0});
    }
    if (totalLength != frame.size()) {
        return Result<void>::Failure(Error{ErrorCode::CorruptData, "IPC frame length mismatch", 0});
    }
    if (typeRaw != static_cast<std::uint32_t>(expectedType)) {
        return Result<void>::Failure(
            Error{ErrorCode::InvalidArgument, "unexpected IPC message type for this decoder", 0});
    }
    if (version != kProtocolVersion) {
        return Result<void>::Failure(Error{ErrorCode::VersionMismatch, "unsupported IPC protocol version", 0});
    }
    return Result<void>::Success();
}

} // namespace

std::vector<std::uint8_t> MessageCodec::EncodeCacheStatisticsResponse(const CacheStatisticsPayload& payload) {
    std::vector<std::uint8_t> frame;
    AppendHeaderPlaceholder(frame, MessageType::GetCacheStatisticsResponse);

    AppendRaw(frame, payload.hitCount);
    AppendRaw(frame, payload.missCount);
    AppendRaw(frame, payload.insertCount);
    AppendRaw(frame, payload.updateCount);
    AppendRaw(frame, payload.invalidationCount);
    AppendRaw(frame, payload.evictionCount);
    AppendRaw(frame, payload.flushSuccessCount);
    AppendRaw(frame, payload.flushFailureCount);
    AppendRaw(frame, payload.currentEntryCount);
    AppendRaw(frame, payload.currentMemoryBytes);
    AppendRaw(frame, payload.dirtyEntryCount);
    AppendRaw(frame, payload.dirtyBytes);

    PatchLength(frame);
    return frame;
}

Result<CacheStatisticsPayload> MessageCodec::DecodeCacheStatisticsResponse(const std::vector<std::uint8_t>& frame) {
    std::size_t offset = 0;
    auto headerCheck = CheckHeader(frame, offset, MessageType::GetCacheStatisticsResponse);
    if (!headerCheck) return Result<CacheStatisticsPayload>::Failure(headerCheck.Err());

    CacheStatisticsPayload payload;
    bool ok = ReadRaw(frame, offset, payload.hitCount) &&
              ReadRaw(frame, offset, payload.missCount) &&
              ReadRaw(frame, offset, payload.insertCount) &&
              ReadRaw(frame, offset, payload.updateCount) &&
              ReadRaw(frame, offset, payload.invalidationCount) &&
              ReadRaw(frame, offset, payload.evictionCount) &&
              ReadRaw(frame, offset, payload.flushSuccessCount) &&
              ReadRaw(frame, offset, payload.flushFailureCount) &&
              ReadRaw(frame, offset, payload.currentEntryCount) &&
              ReadRaw(frame, offset, payload.currentMemoryBytes) &&
              ReadRaw(frame, offset, payload.dirtyEntryCount) &&
              ReadRaw(frame, offset, payload.dirtyBytes);
    if (!ok) {
        return Result<CacheStatisticsPayload>::Failure(
            Error{ErrorCode::CorruptData, "IPC frame truncated cache statistics payload", 0});
    }
    return Result<CacheStatisticsPayload>::Success(payload);
}

std::vector<std::uint8_t> MessageCodec::EncodeOperationResult(MessageType type,
                                                               const OperationResultPayload& payload) {
    std::vector<std::uint8_t> frame;
    AppendHeaderPlaceholder(frame, type);

    std::uint8_t succeededByte = payload.succeeded ? 1 : 0;
    AppendRaw(frame, succeededByte);
    AppendRaw(frame, payload.errorCode);
    AppendString(frame, payload.message);

    PatchLength(frame);
    return frame;
}

Result<OperationResultPayload> MessageCodec::DecodeOperationResult(const std::vector<std::uint8_t>& frame,
                                                                     MessageType expectedType) {
    std::size_t offset = 0;
    auto headerCheck = CheckHeader(frame, offset, expectedType);
    if (!headerCheck) return Result<OperationResultPayload>::Failure(headerCheck.Err());

    OperationResultPayload payload;
    std::uint8_t succeededByte = 0;
    if (!ReadRaw(frame, offset, succeededByte) || !ReadRaw(frame, offset, payload.errorCode) ||
        !ReadString(frame, offset, payload.message)) {
        return Result<OperationResultPayload>::Failure(
            Error{ErrorCode::CorruptData, "IPC frame truncated operation-result payload", 0});
    }
    payload.succeeded = (succeededByte != 0);
    return Result<OperationResultPayload>::Success(payload);
}

std::vector<std::uint8_t> MessageCodec::EncodeInvalidateKeyRequest(const InvalidateKeyRequestPayload& payload) {
    std::vector<std::uint8_t> frame;
    AppendHeaderPlaceholder(frame, MessageType::InvalidateKeyRequest);
    AppendString(frame, payload.key);
    PatchLength(frame);
    return frame;
}

Result<InvalidateKeyRequestPayload> MessageCodec::DecodeInvalidateKeyRequest(const std::vector<std::uint8_t>& frame) {
    std::size_t offset = 0;
    auto headerCheck = CheckHeader(frame, offset, MessageType::InvalidateKeyRequest);
    if (!headerCheck) return Result<InvalidateKeyRequestPayload>::Failure(headerCheck.Err());

    InvalidateKeyRequestPayload payload;
    if (!ReadString(frame, offset, payload.key)) {
        return Result<InvalidateKeyRequestPayload>::Failure(
            Error{ErrorCode::CorruptData, "IPC frame truncated invalidate-key payload", 0});
    }
    return Result<InvalidateKeyRequestPayload>::Success(payload);
}

} // namespace QuantumCache::Ipc
