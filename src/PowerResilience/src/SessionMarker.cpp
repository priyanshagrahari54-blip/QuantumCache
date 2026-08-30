// Real, working implementation of ISessionMarker. Layout of the marker
// file (fixed-size, single record, overwritten in place):
//
//   offset 0  : uint32_t magic       = 0x51434D31 ('Q','C','M','1')
//   offset 4  : uint32_t version     = 1
//   offset 8  : uint64_t generation
//   offset 16 : uint64_t startTimestamp
//   offset 24 : uint8_t  closedCleanly (0 or 1)
//   offset 25 : uint32_t crc32 of bytes [0,25)
//
// The marker is written twice per session (OnSessionStart, OnCleanShutdown)
// and each write is followed by FlushDurable() so a power cut can only ever
// catch it between "not yet updated" (previous state preserved) or "fully
// updated" (new state visible) — never a torn half-write, because the
// record is small enough to fit well within a single filesystem sector and
// we always rewrite the entire record from offset 0.
//
// NOTE: this "no torn write" reasoning holds on real hardware/filesystems
// that provide atomic single-sector writes, which is the common case but
// not a guarantee. Stage 1 tests the STATE MACHINE logic; it does not (and
// cannot, from this sandbox) verify true atomicity guarantees of any real
// disk hardware.
#include "QuantumCache/PowerResilience/ISessionMarker.h"
#include "QuantumCache/Common/Crc32.h"
#include "QuantumCache/Storage/IFile.h"
#include <chrono>
#include <cstring>

namespace QuantumCache::PowerResilience {
namespace {

using Common::Crc32;
using Common::Error;
using Common::ErrorCode;
using Common::Result;

constexpr std::uint32_t kMagic = 0x51434D31u; // "QCM1"
constexpr std::uint32_t kVersion = 1u;
constexpr std::size_t kRecordSize = 29; // 4+4+8+8+1+4

#pragma pack(push, 1)
struct RawRecord {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint64_t generation;
    std::uint64_t startTimestamp;
    std::uint8_t closedCleanly;
    std::uint32_t crc32;
};
#pragma pack(pop)
static_assert(sizeof(RawRecord) == kRecordSize, "RawRecord must be tightly packed");

std::uint64_t NowTicks() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

class SessionMarker final : public ISessionMarker {
public:
    explicit SessionMarker(std::unique_ptr<Storage::IFile> file) : file_(std::move(file)) {}

    Result<SessionMarkerData> ReadLastState() override {
        auto sizeResult = file_->Size();
        if (!sizeResult) {
            return Result<SessionMarkerData>::Failure(sizeResult.Err());
        }
        if (sizeResult.Value() == 0) {
            // Genuinely fresh file (never written to at all, e.g. first
            // ever run): there is nothing to recover, so this is safe to
            // treat as "clean" — no prior session ever existed.
            SessionMarkerData data;
            data.generation = 0;
            data.startTimestamp = 0;
            data.closedCleanly = true;
            return Result<SessionMarkerData>::Success(data);
        }

        if (sizeResult.Value() < kRecordSize) {
            // AUDITED BUG (fixed): a NON-ZERO but short file means a
            // record write was started and torn by a power cut before
            // completing (e.g. the very first OnSessionStart() call was
            // interrupted mid-fwrite, before FlushDurable() ever ran).
            // This used to be lumped in with "no marker written yet" and
            // reported as closedCleanly=true — which is exactly backwards:
            // it silently reports the actual power-loss event that
            // produced this torn file as a clean shutdown, causing
            // RecoveryManager to skip journal replay entirely and lose
            // any pending dirty data from that interrupted session. A
            // short-but-nonzero record can never be trusted for ANY
            // field, so — same reasoning as the CRC-mismatch branch
            // below — report it as an unclean shutdown with generation/
            // timestamp unknown (0), and do not touch
            // lastKnownGeneration_.
            SessionMarkerData data;
            data.generation = 0;
            data.startTimestamp = 0;
            data.closedCleanly = false;
            return Result<SessionMarkerData>::Success(data);
        }

        auto seekResult = file_->Seek(0, false);
        if (!seekResult) return Result<SessionMarkerData>::Failure(seekResult.Err());

        RawRecord raw{};
        auto readResult = file_->Read(&raw, sizeof(raw));
        if (!readResult || readResult.Value() != sizeof(raw)) {
            return Result<SessionMarkerData>::Failure(
                Error{ErrorCode::CorruptData, "session marker truncated", 0});
        }

        if (raw.magic != kMagic || raw.version != kVersion) {
            return Result<SessionMarkerData>::Failure(
                Error{ErrorCode::CorruptData, "session marker bad magic/version", 0});
        }

        std::uint32_t expectedCrc = Crc32::Compute(&raw, offsetof(RawRecord, crc32));
        if (expectedCrc != raw.crc32) {
            // AUDITED BUG (fixed): the marker itself was torn by a power
            // cut mid-write. Since we cannot trust either the old or new
            // logical value, the safe interpretation is "unclean
            // shutdown", never "clean" — that part was already correct.
            // What was NOT correct: this branch used to copy
            // raw.generation/raw.startTimestamp out of the very record
            // that just failed CRC validation, and even fed
            // raw.generation into lastKnownGeneration_ (which
            // OnSessionStart() later increments from). A CRC-invalid
            // record's bytes cannot be trusted for ANY field — magic,
            // version, generation, timestamp, or dirty/clean flag — not
            // just the flag we happen to care most about, because a
            // torn write can corrupt any subset of the record's bytes.
            // Do not read generation/timestamp from an untrusted record;
            // report them as unknown (0) instead, and do NOT update
            // lastKnownGeneration_ from untrusted data (leave it exactly
            // as it already was — 0 unless some earlier, validated read
            // or write already established a real value in this
            // process), so a subsequent OnSessionStart() still produces
            // a well-defined, monotonic-from-this-process's-own-
            // perspective generation rather than continuing a
            // potentially fabricated counter.
            SessionMarkerData data;
            data.generation = 0;
            data.startTimestamp = 0;
            data.closedCleanly = false;
            return Result<SessionMarkerData>::Success(data);
        }

        SessionMarkerData data;
        data.generation = raw.generation;
        data.startTimestamp = raw.startTimestamp;
        data.closedCleanly = (raw.closedCleanly != 0);
        lastKnownGeneration_ = raw.generation;
        return Result<SessionMarkerData>::Success(data);
    }

    Result<void> OnSessionStart() override {
        RawRecord raw{};
        raw.magic = kMagic;
        raw.version = kVersion;
        raw.generation = ++lastKnownGeneration_;
        raw.startTimestamp = NowTicks();
        raw.closedCleanly = 0;
        raw.crc32 = Crc32::Compute(&raw, offsetof(RawRecord, crc32));
        return WriteRecord(raw);
    }

    Result<void> OnCleanShutdown() override {
        RawRecord raw{};
        raw.magic = kMagic;
        raw.version = kVersion;
        raw.generation = lastKnownGeneration_;
        raw.startTimestamp = NowTicks();
        raw.closedCleanly = 1;
        raw.crc32 = Crc32::Compute(&raw, offsetof(RawRecord, crc32));
        return WriteRecord(raw);
    }

    std::uint64_t CurrentGeneration() const noexcept override { return lastKnownGeneration_; }

private:
    Result<void> WriteRecord(const RawRecord& raw) {
        auto seekResult = file_->Seek(0, false);
        if (!seekResult) return Result<void>::Failure(seekResult.Err());

        auto writeResult = file_->Write(&raw, sizeof(raw));
        if (!writeResult || writeResult.Value() != sizeof(raw)) {
            return Result<void>::Failure(
                Error{ErrorCode::IoError, "failed to write full session marker record", 0});
        }

        return file_->FlushDurable();
    }

    std::unique_ptr<Storage::IFile> file_;
    std::uint64_t lastKnownGeneration_{0};
};

} // namespace

Common::Result<std::unique_ptr<ISessionMarker>> CreateSessionMarker(
    std::unique_ptr<Storage::IFile> file) {
    if (!file) {
        return Common::Result<std::unique_ptr<ISessionMarker>>::Failure(
            Error{ErrorCode::InvalidArgument, "file must not be null", 0});
    }
    return Common::Result<std::unique_ptr<ISessionMarker>>::Success(
        std::make_unique<SessionMarker>(std::move(file)));
}

} // namespace QuantumCache::PowerResilience
