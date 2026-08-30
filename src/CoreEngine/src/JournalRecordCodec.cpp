#include "QuantumCache/CoreEngine/JournalRecordCodec.h"
#include <cstring>

namespace QuantumCache::CoreEngine {
namespace {

using Common::Error;
using Common::ErrorCode;
using Common::Result;

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

bool ReadBytes(const std::vector<std::uint8_t>& buffer, std::size_t& offset,
               std::uint32_t length, std::vector<std::uint8_t>& out) {
    if (offset + length > buffer.size()) return false;
    out.assign(buffer.begin() + static_cast<long>(offset),
               buffer.begin() + static_cast<long>(offset + length));
    offset += length;
    return true;
}

} // namespace

std::vector<std::uint8_t> JournalRecordCodec::Encode(const CacheJournalRecord& record) {
    std::vector<std::uint8_t> buffer;
    buffer.reserve(32 + record.key.size() + record.value.size());

    AppendRaw(buffer, record.formatVersion);
    AppendRaw(buffer, static_cast<std::uint32_t>(record.type));
    AppendRaw(buffer, record.entryVersion);

    AppendRaw(buffer, static_cast<std::uint32_t>(record.key.size()));
    buffer.insert(buffer.end(), record.key.begin(), record.key.end());

    AppendRaw(buffer, static_cast<std::uint32_t>(record.value.size()));
    buffer.insert(buffer.end(), record.value.begin(), record.value.end());

    return buffer;
}

Result<CacheJournalRecord> JournalRecordCodec::Decode(const std::vector<std::uint8_t>& payload) {
    std::size_t offset = 0;
    CacheJournalRecord record;
    std::uint32_t typeRaw = 0;
    std::uint32_t keyLength = 0;
    std::uint32_t valueLength = 0;

    if (!ReadRaw(payload, offset, record.formatVersion) ||
        !ReadRaw(payload, offset, typeRaw) ||
        !ReadRaw(payload, offset, record.entryVersion) ||
        !ReadRaw(payload, offset, keyLength)) {
        return Result<CacheJournalRecord>::Failure(
            Error{ErrorCode::CorruptData, "cache journal record: header truncated", 0});
    }

    if (record.formatVersion != kCacheRecordFormatVersion) {
        return Result<CacheJournalRecord>::Failure(
            Error{ErrorCode::VersionMismatch, "cache journal record: unsupported formatVersion", 0});
    }

    if (typeRaw < static_cast<std::uint32_t>(CacheRecordType::Upsert) ||
        typeRaw > static_cast<std::uint32_t>(CacheRecordType::Invalidate)) {
        return Result<CacheJournalRecord>::Failure(
            Error{ErrorCode::CorruptData, "cache journal record: invalid record type", 0});
    }
    record.type = static_cast<CacheRecordType>(typeRaw);

    // Sanity bound: refuse to allocate an absurd key/value length caused
    // by decoding garbage. 64 MiB is far beyond any realistic single
    // cache entry Stage 2 deals with while still not being an arbitrary
    // "1" that would reject legitimate data.
    constexpr std::uint32_t kMaxReasonableLength = 64u * 1024u * 1024u;

    std::vector<std::uint8_t> keyBytes;
    if (keyLength > kMaxReasonableLength || !ReadBytes(payload, offset, keyLength, keyBytes)) {
        return Result<CacheJournalRecord>::Failure(
            Error{ErrorCode::CorruptData, "cache journal record: key truncated or unreasonable", 0});
    }
    record.key.assign(keyBytes.begin(), keyBytes.end());

    if (!ReadRaw(payload, offset, valueLength)) {
        return Result<CacheJournalRecord>::Failure(
            Error{ErrorCode::CorruptData, "cache journal record: missing value length", 0});
    }
    if (valueLength > kMaxReasonableLength ||
        !ReadBytes(payload, offset, valueLength, record.value)) {
        return Result<CacheJournalRecord>::Failure(
            Error{ErrorCode::CorruptData, "cache journal record: value truncated or unreasonable", 0});
    }

    return Result<CacheJournalRecord>::Success(record);
}

} // namespace QuantumCache::CoreEngine
