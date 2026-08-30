#pragma once
#include "QuantumCache/Common/Result.h"
#include "QuantumCache/CoreEngine/JournalRecords.h"
#include <cstdint>
#include <vector>

namespace QuantumCache::CoreEngine {

// Encodes/decodes CacheJournalRecord to/from the opaque byte payload
// IWriteAheadJournal::Append()/Replay() carry. Kept separate from
// CacheEngine so the wire format can be unit tested in isolation, mirroring
// how QuantumCache::Ipc::MessageCodec is split from the transport.
//
// On-disk payload layout (independent of, and nested inside, the journal
// frame's own CRC-32 framing — see IWriteAheadJournal.h):
//   uint32_t formatVersion
//   uint32_t recordType       (CacheRecordType)
//   uint64_t entryVersion
//   uint32_t keyLength
//   uint8_t  key[keyLength]
//   uint32_t valueLength      (0 for record types that carry no value)
//   uint8_t  value[valueLength]
class JournalRecordCodec {
public:
    [[nodiscard]] static std::vector<std::uint8_t> Encode(const CacheJournalRecord& record);

    [[nodiscard]] static Common::Result<CacheJournalRecord> Decode(
        const std::vector<std::uint8_t>& payload);
};

} // namespace QuantumCache::CoreEngine
