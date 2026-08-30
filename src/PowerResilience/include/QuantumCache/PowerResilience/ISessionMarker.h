#pragma once
#include "QuantumCache/Common/Result.h"
#include <cstdint>
#include <memory>
#include <string>

namespace QuantumCache::Storage { class IFile; }

namespace QuantumCache::PowerResilience {

// A durable, small on-disk marker recording whether the last QuantumCache
// session ended cleanly. This is the primary power-loss detection
// mechanism: on startup we open the marker file and inspect its recorded
// state BEFORE touching anything else; if it says "open" (was never
// transitioned to Closed), the previous run did not shut down in an
// orderly way — for this laptop that most often means a power cut.
struct SessionMarkerData {
    // Monotonically increasing per-process-start counter, persisted so
    // repeated crashes can be observed/counted rather than only detected
    // once.
    std::uint64_t generation{0};

    // Opaque, implementation-defined timestamp (100ns ticks since epoch on
    // Windows via GetSystemTimePreciseAsFileTime; wall clock time_t-based
    // value on the portable reference build). Only used for diagnostics in
    // Stage 1, not for correctness.
    std::uint64_t startTimestamp{0};

    // True once OnCleanShutdown() has been recorded for this generation.
    bool closedCleanly{false};
};

class ISessionMarker {
public:
    virtual ~ISessionMarker() = default;

    // Reads the marker as it was left by the previous process, WITHOUT
    // modifying it. Returns Ok even if no marker existed yet (generation
    // will be 0, closedCleanly will be true, meaning "nothing to recover").
    [[nodiscard]] virtual Common::Result<SessionMarkerData> ReadLastState() = 0;

    // Records the start of a new session: increments generation, sets
    // closedCleanly=false, and durably flushes to disk. Must be called
    // and durably committed BEFORE any cache/journal writes for the new
    // session begin, so a crash immediately after startup is still
    // detected on the next run.
    [[nodiscard]] virtual Common::Result<void> OnSessionStart() = 0;

    // Records an orderly shutdown for the current generation and durably
    // flushes to disk. Must be the last durable write performed before
    // process exit.
    [[nodiscard]] virtual Common::Result<void> OnCleanShutdown() = 0;

    [[nodiscard]] virtual std::uint64_t CurrentGeneration() const noexcept = 0;
};

// Factory: wraps an already-opened durable file (see Storage::OpenFile) as
// a session marker. Kept as a factory (rather than a constructor call
// site) so callers depend only on the interface header.
[[nodiscard]] Common::Result<std::unique_ptr<ISessionMarker>> CreateSessionMarker(
    std::unique_ptr<Storage::IFile> file);

} // namespace QuantumCache::PowerResilience
