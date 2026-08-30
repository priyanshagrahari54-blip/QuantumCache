#pragma once
#include <cstdint>
#include <string>

namespace QuantumCache::Ipc {

// Wire protocol between the WinUI 3 GUI (client) and the Windows Service
// (server), exchanged over a named pipe.
//
// Stage 1 defined only enough to reflect service/recovery status (there
// was no cache engine to control). Stage 2 adds messages for the cache
// management surface that now genuinely exists: reading real statistics
// (CoreEngine::CacheStatistics — actual counters, never invented),
// triggering a real FlushAll(), and invalidating a specific key. It
// deliberately does NOT add messages for functionality that still does
// not exist (no Get/Put data-plane access over IPC — the GUI is a
// status/management surface, not a cache client; no SSD-tier or
// predictive-cache controls).
enum class MessageType : std::uint32_t {
    // Client -> Server
    GetStatusRequest = 1,

    // Server -> Client
    GetStatusResponse = 2,

    // Server -> Client (unsolicited), sent whenever RecoveryState changes,
    // in particular so the GUI can surface "recovering from an unexpected
    // shutdown" to the user in real time.
    RecoveryStateChanged = 3,

    // --- Stage 2: cache management ---

    // Client -> Server. Requests the real, current CacheStatistics
    // snapshot (hit/miss counters, entry/dirty counts, memory bytes).
    GetCacheStatisticsRequest = 4,
    // Server -> Client.
    GetCacheStatisticsResponse = 5,

    // Client -> Server. Requests ICacheEngine::FlushAll(). The response
    // reports whether it fully succeeded — this is a real operation, not
    // an acknowledgement-only placeholder.
    FlushAllRequest = 6,
    // Server -> Client.
    FlushAllResponse = 7,

    // Client -> Server. Requests ICacheEngine::ForceInvalidate(key) for
    // one specific key (an explicit, deliberate operation — the plain,
    // non-forced Invalidate() semantics that refuse on dirty data are not
    // exposed over IPC in Stage 2, since a remote GUI caller cannot make
    // an informed "override" decision the way in-process engine code can;
    // ForceInvalidate is exposed because it has one unambiguous meaning:
    // "remove this key, accepting loss of any unflushed value").
    InvalidateKeyRequest = 8,
    // Server -> Client.
    InvalidateKeyResponse = 9,

    // Either direction, sent on protocol errors (e.g. unknown message
    // type, malformed frame) instead of silently dropping the connection.
    ProtocolError = 0xFFFFFFFF,
};

// Every message on the wire is length-prefixed:
//   uint32_t totalFrameLength (including this field)
//   uint32_t messageType (MessageType)
//   uint32_t protocolVersion
//   <message-specific fields>
// Kept as a fixed, versioned header from Stage 1 onward specifically so
// the GUI and Service can be updated independently later without silently
// desyncing.
//
// Bumped to 2 for Stage 2's new message types. A version-1 client talking
// to a version-2 server (or vice versa) is rejected by
// MessageCodec::Decode* with ErrorCode::VersionMismatch rather than
// silently misinterpreted — see MessageCodecTest for the regression test.
constexpr std::uint32_t kProtocolVersion = 2;

struct StatusResponsePayload {
    // Mirrors PowerResilience::RecoveryState numeric values (kept as a
    // plain uint32_t here so Ipc does not need to depend on
    // PowerResilience just for an enum — the Service is responsible for
    // keeping this in sync, verified by IpcProtocolTests).
    std::uint32_t recoveryState{0};
    std::uint64_t sessionGeneration{0};
    // Stage 2: reflects whether ICacheEngine::MarkRecoveryComplete() has
    // actually been called successfully by the Service — never hardcoded.
    bool cacheEngineActive{false};
};

// Mirrors CoreEngine::CacheStatistics field-for-field. Kept as an
// independent Ipc-owned struct (rather than #include-ing CoreEngine's
// header) for the same layering reason StatusResponsePayload keeps its
// own recoveryState field: Ipc must not depend on CoreEngine, and the
// Service is responsible for keeping the two in sync — verified by
// IpcCodecTests and by CacheStatusBridgeTests.
struct CacheStatisticsPayload {
    std::uint64_t hitCount{0};
    std::uint64_t missCount{0};
    std::uint64_t insertCount{0};
    std::uint64_t updateCount{0};
    std::uint64_t invalidationCount{0};
    std::uint64_t evictionCount{0};
    std::uint64_t flushSuccessCount{0};
    std::uint64_t flushFailureCount{0};
    std::uint64_t currentEntryCount{0};
    std::uint64_t currentMemoryBytes{0};
    std::uint64_t dirtyEntryCount{0};
    std::uint64_t dirtyBytes{0};
};

// Generic ack payload for operations whose result is just success/failure
// plus a human-readable message (FlushAllResponse, InvalidateKeyResponse).
// Mirrors Common::Error's shape narrowly (code + message) rather than
// depending on Common::Error directly, keeping Ipc's wire types
// self-contained and independently versionable.
struct OperationResultPayload {
    bool succeeded{false};
    std::uint32_t errorCode{0}; // Common::ErrorCode numeric value; 0 (Ok) when succeeded
    std::string message;
};

struct InvalidateKeyRequestPayload {
    std::string key;
};

} // namespace QuantumCache::Ipc
