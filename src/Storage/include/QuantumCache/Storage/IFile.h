#pragma once
#include "QuantumCache/Common/Result.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace QuantumCache::Storage {

enum class OpenMode : std::uint32_t {
    // Open existing file for read/write; fail if it does not exist.
    OpenExisting,
    // Create a new file, failing if one already exists.
    CreateNew,
    // Open existing file, or create it if it does not exist.
    OpenOrCreate,
};

// Durable-file abstraction used by everything in Stage 1 that needs
// crash-consistent persistence (session marker, write-ahead journal,
// configuration store). This is intentionally narrow: it is NOT a cache
// storage engine. It exists so the power-resilience and configuration
// components do not depend on platform file APIs directly, and so the
// real Win32 implementation (CreateFileW/WriteFile/FlushFileBuffers) and a
// portable implementation (std::fstream, used for host-side testing and
// non-Windows development) can be swapped behind one interface.
class IFile {
public:
    virtual ~IFile() = default;

    [[nodiscard]] virtual Common::Result<std::size_t> Read(void* buffer, std::size_t bytes) = 0;
    [[nodiscard]] virtual Common::Result<std::size_t> Write(const void* buffer, std::size_t bytes) = 0;

    // Moves the file position. Returns the new absolute offset.
    [[nodiscard]] virtual Common::Result<std::uint64_t> Seek(std::int64_t offset, bool fromEnd) = 0;

    [[nodiscard]] virtual Common::Result<std::uint64_t> Size() const = 0;

    // Truncates (or extends with undefined content) the file to exactly
    // `length` bytes. Added in Stage 2 for real write-ahead journal
    // compaction/truncation (Win32 SetEndOfFile; ftruncate on the
    // portable reference build) — Stage 1 left this as a known gap
    // because nothing needed it yet; the cache engine's journal
    // compaction does. Callers that care about durability of the new
    // length must still call FlushDurable() afterwards.
    //
    // FILE POSITION CONTRACT (audited/clarified in Stage 2 hardening):
    // the file position after SetLength() returns is UNSPECIFIED and
    // MAY DIFFER between implementations — Win32File currently leaves it
    // at `length` (a side effect of how SetEndOfFile is implemented, via
    // SetFilePointerEx to `length` first), while PortableFile explicitly
    // repositions to 0. Neither behavior is part of the contract. Every
    // caller in this codebase already calls Seek() to an explicit offset
    // immediately after SetLength() before any Read()/Write() (see
    // WriteAheadJournal::Truncate() and FileBackingStore's replay-time
    // truncation) specifically because of this — do not add a new
    // caller that reads/writes immediately after SetLength() without an
    // intervening explicit Seek().
    [[nodiscard]] virtual Common::Result<void> SetLength(std::uint64_t length) = 0;

    // Forces data to stable storage (FlushFileBuffers on Win32; fsync/fflush
    // on the portable reference implementation). This is what makes the
    // journal and session marker crash-consistent: callers MUST call this
    // after any write whose durability matters before considering the
    // write "committed".
    [[nodiscard]] virtual Common::Result<void> FlushDurable() = 0;

    virtual void Close() = 0;
};

// Factory for the platform-appropriate IFile implementation.
// - On a real Windows build (_WIN32 defined, whether compiled by MSVC or
//   cross-compiled with MinGW-w64), this returns the Win32File
//   implementation backed by CreateFileW/WriteFile/FlushFileBuffers.
// - On any other platform, it returns PortableFile, which is a real,
//   working implementation used ONLY so Stage 1's crash-consistency logic
//   can be unit tested on non-Windows development/CI machines. It is not a
//   substitute for validating on real Windows/NTFS.
[[nodiscard]] Common::Result<std::unique_ptr<IFile>> OpenFile(const std::wstring& path, OpenMode mode);

} // namespace QuantumCache::Storage
