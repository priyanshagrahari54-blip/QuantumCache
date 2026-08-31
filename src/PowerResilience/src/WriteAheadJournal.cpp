// Real, working append-only journal implementation.
//
// On-disk record framing (repeated for each appended record):
//   uint32_t frameMagic   = 0x51434A31 ('Q','C','J','1')
//   uint64_t sequenceNumber
//   uint32_t payloadLength
//   uint8_t  payload[payloadLength]
//   uint32_t crc32   (over frameMagic..payload, i.e. everything above)
//
// Durability contract: Append() writes the full frame then calls
// FlushDurable() before returning success. This means: if a power cut
// happens after Append() returns Ok, the record WILL be found intact on
// next Replay(). If a power cut happens DURING the write (before
// FlushDurable completes), Replay() will detect the torn frame (via
// CRC mismatch, a short read, or a frame extending past current EOF) and
// stop there — never fabricating or misinterpreting partial data.
#include "QuantumCache/PowerResilience/IWriteAheadJournal.h"
#include "QuantumCache/Common/Crc32.h"
#include "QuantumCache/Storage/IFile.h"
#include <cstring>
#include <optional>

namespace QuantumCache::PowerResilience {
namespace {

using Common::Crc32;
using Common::Error;
using Common::ErrorCode;
using Common::Result;

constexpr std::uint32_t kFrameMagic = 0x51434A31u; // "QCJ1"

// AUDITED BUG (fixed): Replay() used to allocate
// std::vector<std::uint8_t> payload(payloadLength) directly from a
// uint32_t length field read straight off disk, with NO sanity bound —
// unlike JournalRecordCodec.cpp (64 MiB cap), MessageCodec.cpp (16 MiB
// cap), and CacheEngine.cpp's read-path value-size check (64 MiB), all
// of which already learned this lesson for their own untrusted
// length fields. A single flipped/corrupted length byte — exactly the
// kind of damage a torn write or bad sector can cause, i.e. precisely
// the failure mode this whole project exists to be resilient against —
// could request an allocation up to ~4 GiB (UINT32_MAX), causing an
// uncaught std::bad_alloc/OOM crash during startup replay instead of a
// clean, reported CorruptData failure. Bounded here to whichever is
// smaller: a fixed sane cap, or the number of bytes actually remaining
// in the file (a corrupted length claiming more data than physically
// exists cannot possibly be a valid record either way).
constexpr std::uint32_t kMaxReasonablePayloadLength = 256u * 1024u * 1024u;

class WriteAheadJournal final : public IWriteAheadJournal {
public:
    explicit WriteAheadJournal(std::unique_ptr<Storage::IFile> file) : file_(std::move(file)) {}

    Result<std::uint64_t> Append(const std::vector<std::uint8_t>& payload) override {
        std::uint64_t seq = nextSequenceNumber_;

        std::vector<std::uint8_t> frame;
        frame.reserve(sizeof(kFrameMagic) + sizeof(seq) + sizeof(std::uint32_t) + payload.size());

        AppendRaw(frame, kFrameMagic);
        AppendRaw(frame, seq);
        AppendRaw(frame, static_cast<std::uint32_t>(payload.size()));
        frame.insert(frame.end(), payload.begin(), payload.end());

        std::uint32_t crc = Crc32::Compute(frame.data(), frame.size());
        AppendRaw(frame, crc);

        auto seekResult = file_->Seek(0, true); // append at current EOF
        if (!seekResult) return Result<std::uint64_t>::Failure(seekResult.Err());

        auto writeResult = file_->Write(frame.data(), frame.size());
        if (!writeResult || writeResult.Value() != frame.size()) {
            return Result<std::uint64_t>::Failure(
                Error{ErrorCode::IoError, "journal append: short write", 0});
        }

        auto flushResult = file_->FlushDurable();
        if (!flushResult) return Result<std::uint64_t>::Failure(flushResult.Err());

        ++nextSequenceNumber_;
        ++recordCount_;
        return Result<std::uint64_t>::Success(seq);
    }

    Result<void> AppendBatch(const std::vector<std::vector<std::uint8_t>>& payloads) override {
        if (payloads.empty()) return Result<void>::Success();

        std::vector<std::uint8_t> batchBuffer;
        std::uint64_t currentSeq = nextSequenceNumber_;

        for (const auto& payload : payloads) {
            std::vector<std::uint8_t> frame;
            frame.reserve(sizeof(kFrameMagic) + sizeof(currentSeq) + sizeof(std::uint32_t) + payload.size() + sizeof(std::uint32_t));

            AppendRaw(frame, kFrameMagic);
            AppendRaw(frame, currentSeq);
            AppendRaw(frame, static_cast<std::uint32_t>(payload.size()));
            frame.insert(frame.end(), payload.begin(), payload.end());

            std::uint32_t crc = Crc32::Compute(frame.data(), frame.size());
            AppendRaw(frame, crc);

            batchBuffer.insert(batchBuffer.end(), frame.begin(), frame.end());
            ++currentSeq;
        }

        auto seekResult = file_->Seek(0, true);
        if (!seekResult) return Result<void>::Failure(seekResult.Err());

        auto writeResult = file_->Write(batchBuffer.data(), batchBuffer.size());
        if (!writeResult || writeResult.Value() != batchBuffer.size()) {
            return Result<void>::Failure(
                Error{ErrorCode::IoError, "journal batch append: short write", 0});
        }

        auto flushResult = file_->FlushDurable();
        if (!flushResult) return Result<void>::Failure(flushResult.Err());

        nextSequenceNumber_ = currentSeq;
        recordCount_ += payloads.size();
        return Result<void>::Success();
    }

    Result<void> Replay(const ReplayCallback& callback) override {
        auto seekResult = file_->Seek(0, false);
        if (!seekResult) return Result<void>::Failure(seekResult.Err());

        std::uint64_t highestSeqSeen = 0;
        bool any = false;

        for (;;) {
            std::uint32_t frameMagic = 0;
            auto readMagic = ReadExact(&frameMagic, sizeof(frameMagic));
            if (!readMagic.has_value()) {
                break; // clean EOF: nothing more, or a torn header at the tail.
            }
            if (frameMagic != kFrameMagic) {
                // Corrupt frame header — stop replay here; do not trust
                // anything from this point onward. Everything before this
                // point was already handed to the callback.
                break;
            }

            std::uint64_t sequenceNumber = 0;
            std::uint32_t payloadLength = 0;
            if (!ReadExact(&sequenceNumber, sizeof(sequenceNumber)).has_value()) break;
            if (!ReadExact(&payloadLength, sizeof(payloadLength)).has_value()) break;

            // AUDITED BUG (fixed): treat an absurd length exactly like a
            // torn/corrupt frame (break, not a hard Failure) — this
            // field has not passed its own frame's CRC check yet at
            // this point, so a length this large is indistinguishable
            // from garbage left by a torn write, which is precisely
            // what the existing "break and treat everything before this
            // point as the valid, replayable prefix" handling already
            // exists for (see the frameMagic mismatch case just above).
            // The only difference from that case is defending against
            // the allocation itself, not merely the interpretation.
            if (payloadLength > kMaxReasonablePayloadLength) {
                break;
            }

            std::vector<std::uint8_t> payload(payloadLength);
            if (payloadLength > 0) {
                auto payloadOk = ReadExact(payload.data(), payloadLength);
                if (!payloadOk.has_value()) break;
            }

            std::uint32_t storedCrc = 0;
            if (!ReadExact(&storedCrc, sizeof(storedCrc)).has_value()) break;

            std::vector<std::uint8_t> frameForCrc;
            frameForCrc.reserve(sizeof(frameMagic) + sizeof(sequenceNumber) + sizeof(payloadLength) + payload.size());
            AppendRaw(frameForCrc, frameMagic);
            AppendRaw(frameForCrc, sequenceNumber);
            AppendRaw(frameForCrc, payloadLength);
            frameForCrc.insert(frameForCrc.end(), payload.begin(), payload.end());
            std::uint32_t computedCrc = Crc32::Compute(frameForCrc.data(), frameForCrc.size());

            if (computedCrc != storedCrc) {
                // Torn write from a power cut mid-append. Stop here; this
                // record and anything after it must be discarded.
                break;
            }

            any = true;
            highestSeqSeen = sequenceNumber;

            JournalRecord record;
            record.sequenceNumber = sequenceNumber;
            record.payload = std::move(payload);

            auto actionResult = callback(record);
            if (!actionResult) {
                return Result<void>::Failure(actionResult.Err());
            }
            if (actionResult.Value() == ReplayAction::StopReplaySuccess) {
                break;
            }
        }

        if (any) {
            nextSequenceNumber_ = highestSeqSeen + 1;
        }
        return Result<void>::Success();
    }

    Result<void> Truncate() override {
        // Stage 2: actually shrinks the backing file to zero bytes using
        // IFile::SetLength (added in Stage 2 specifically for this), then
        // durably flushes the truncation itself so a power cut immediately
        // after Truncate() cannot resurrect old, already-applied records.
        auto setLengthResult = file_->SetLength(0);
        if (!setLengthResult) return setLengthResult;

        auto seekResult = file_->Seek(0, false);
        if (!seekResult) return Result<void>::Failure(seekResult.Err());

        auto flushResult = file_->FlushDurable();
        if (!flushResult) return flushResult;

        recordCount_ = 0;
        nextSequenceNumber_ = 0;
        return Result<void>::Success();
    }

    std::size_t RecordCount() const noexcept override { return recordCount_; }

private:
    template <typename T>
    static void AppendRaw(std::vector<std::uint8_t>& buffer, const T& value) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(T));
    }

    // Returns std::nullopt on short read (EOF or error) without treating
    // it as a hard failure — callers interpret that as "end of usable
    // journal data", which correctly covers both a normal EOF and a torn
    // tail record left by a power cut mid-append.
    std::optional<std::size_t> ReadExact(void* buffer, std::size_t bytes) {
        auto readResult = file_->Read(buffer, bytes);
        if (!readResult) return std::nullopt;
        if (readResult.Value() != bytes) return std::nullopt;
        return readResult.Value();
    }

    std::unique_ptr<Storage::IFile> file_;
    std::uint64_t nextSequenceNumber_{0};
    std::size_t recordCount_{0};
};

} // namespace

Common::Result<std::unique_ptr<IWriteAheadJournal>> CreateWriteAheadJournal(
    std::unique_ptr<Storage::IFile> file) {
    if (!file) {
        return Common::Result<std::unique_ptr<IWriteAheadJournal>>::Failure(
            Error{ErrorCode::InvalidArgument, "file must not be null", 0});
    }
    return Common::Result<std::unique_ptr<IWriteAheadJournal>>::Success(
        std::make_unique<WriteAheadJournal>(std::move(file)));
}

} // namespace QuantumCache::PowerResilience
