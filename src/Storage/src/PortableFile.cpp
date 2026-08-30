// Portable reference implementation of IFile using standard C file I/O.
// Compiled ONLY when NOT targeting Windows (i.e. on this Linux development
// sandbox). Its sole purpose is to let PowerResilience/Configuration/Storage
// logic be unit tested with GoogleTest on a non-Windows host. It is not a
// product component and must never be linked into a Windows build — the
// real product always uses Win32File (see Win32File.cpp) on Windows.
#include "QuantumCache/Storage/IFile.h"
#include <cstdio>
#include <string>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace QuantumCache::Storage {
namespace {

using Common::Error;
using Common::ErrorCode;
using Common::Result;

std::string NarrowPath(const std::wstring& wide) {
    std::string narrow;
    narrow.reserve(wide.size());
    for (wchar_t c : wide) {
        narrow.push_back(static_cast<char>(c));
    }
    return narrow;
}

class PortableFile final : public IFile {
public:
    explicit PortableFile(std::FILE* fp) : fp_(fp) {}
    ~PortableFile() override { Close(); }

    PortableFile(const PortableFile&) = delete;
    PortableFile& operator=(const PortableFile&) = delete;

    Result<std::size_t> Read(void* buffer, std::size_t bytes) override {
        std::size_t got = std::fread(buffer, 1, bytes, fp_);
        if (got < bytes && std::ferror(fp_)) {
            return Result<std::size_t>::Failure(
                Error{ErrorCode::IoError, "fread failed", 0});
        }
        return Result<std::size_t>::Success(got);
    }

    Result<std::size_t> Write(const void* buffer, std::size_t bytes) override {
        std::size_t written = std::fwrite(buffer, 1, bytes, fp_);
        if (written != bytes) {
            return Result<std::size_t>::Failure(
                Error{ErrorCode::IoError, "fwrite failed", 0});
        }
        return Result<std::size_t>::Success(written);
    }

    Result<std::uint64_t> Seek(std::int64_t offset, bool fromEnd) override {
        if (std::fseek(fp_, static_cast<long>(offset), fromEnd ? SEEK_END : SEEK_SET) != 0) {
            return Result<std::uint64_t>::Failure(
                Error{ErrorCode::IoError, "fseek failed", 0});
        }
        long pos = std::ftell(fp_);
        if (pos < 0) {
            return Result<std::uint64_t>::Failure(
                Error{ErrorCode::IoError, "ftell failed", 0});
        }
        return Result<std::uint64_t>::Success(static_cast<std::uint64_t>(pos));
    }

    Result<std::uint64_t> Size() const override {
        long current = std::ftell(fp_);
        std::fseek(fp_, 0, SEEK_END);
        long end = std::ftell(fp_);
        std::fseek(fp_, current, SEEK_SET);
        if (end < 0) {
            return Result<std::uint64_t>::Failure(
                Error{ErrorCode::IoError, "ftell failed", 0});
        }
        return Result<std::uint64_t>::Success(static_cast<std::uint64_t>(end));
    }

    Result<void> SetLength(std::uint64_t length) override {
#if defined(__unix__) || defined(__APPLE__)
        std::fflush(fp_);
        if (::ftruncate(fileno(fp_), static_cast<off_t>(length)) != 0) {
            return Result<void>::Failure(Error{ErrorCode::IoError, "ftruncate failed", 0});
        }
        // Reposition to a valid offset within the new length so subsequent
        // Seek()/Read()/Write() calls behave consistently with Win32's
        // SetEndOfFile (which does not itself move the file pointer past
        // what the caller explicitly requested).
        std::fseek(fp_, 0, SEEK_SET);
        return Result<void>::Success();
#else
        (void)length;
        return Result<void>::Failure(
            Error{ErrorCode::PlatformUnsupported, "SetLength not implemented on this platform", 0});
#endif
    }

    Result<void> FlushDurable() override {
        // AUDITED BUG (fixed): this used to call ONLY std::fflush(),
        // which merely moves bytes from libc's userspace stdio buffer
        // into the OS page cache. It does NOT request that the OS
        // persist those bytes to physical, non-volatile storage — a
        // real power cut can still lose data that fflush() already
        // "succeeded" on, because the page cache itself is volatile.
        // Three genuinely different things, which must not be confused
        // with one another (see the file-header note above and
        // docs/STAGE2_ARCHITECTURE.md "Durability terminology" for the
        // full three-way distinction this project now documents
        // everywhere FlushDurable is discussed):
        //   1. Normal flush (fflush): userspace buffer -> OS page cache.
        //      Survives THIS PROCESS crashing; does NOT survive a power
        //      cut or OS crash.
        //   2. OS durability request (fsync/fdatasync, FlushFileBuffers
        //      on Windows): asks the OS to push the data from the page
        //      cache through to the storage device, and — for devices
        //      that honor the request — to flush the device's own
        //      volatile write cache. This is the load-bearing call for
        //      the "survive a real power cut" contract IWriteAheadJournal
        //      and ISessionMarker depend on.
        //   3. Actual physical power-loss testing: literally cutting
        //      power to real hardware while a write is in flight and
        //      confirming the expected data is (or is not) present after
        //      reboot. Nothing in this codebase performs or claims to
        //      have performed that test — see docs/ENVIRONMENT.md.
        // This implementation now performs step 2 for real on POSIX
        // (fflush to push libc's buffer into the OS, then fsync to
        // request the OS/device actually persist it), which is the
        // correct behavior for the interface's documented contract
        // ("Forces data to stable storage ... fsync/fflush"). Whether
        // the underlying device/filesystem HONORS an fsync request
        // (rather than silently no-op'ing, which some virtualized or
        // network filesystems do) is outside what any user-mode call can
        // control or verify — that is exactly why actual physical
        // power-loss testing (step 3) is a distinct, additional
        // requirement this reference implementation does not and cannot
        // satisfy from a development sandbox.
        if (std::fflush(fp_) != 0) {
            return Result<void>::Failure(Error{ErrorCode::IoError, "fflush failed", 0});
        }
#if defined(__unix__) || defined(__APPLE__)
        int fd = fileno(fp_);
        if (fd < 0) {
            return Result<void>::Failure(Error{ErrorCode::IoError, "fileno failed", 0});
        }
        // fsync (not fdatasync) is used deliberately: this file may have
        // had its length changed (SetLength/ftruncate), and fdatasync is
        // not guaranteed to persist file-size metadata, only content —
        // fsync persists both, matching FlushFileBuffers' Windows
        // semantics of durability that includes any pending metadata
        // relevant to reading the data back correctly.
        if (::fsync(fd) != 0) {
            return Result<void>::Failure(Error{ErrorCode::IoError, "fsync failed", 0});
        }
        return Result<void>::Success();
#else
        // No POSIX fsync available on this platform. Rather than
        // silently claiming durability that was not actually requested
        // from the OS, report this configuration as unsupported for the
        // durability contract FlushDurable() promises. This reference
        // implementation exists solely to unit test crash-recovery LOGIC
        // on non-Windows hosts (see file header); real product durability
        // on Windows is provided by Win32File::FlushDurable
        // (FlushFileBuffers), not by this class.
        return Result<void>::Failure(
            Error{ErrorCode::PlatformUnsupported,
                  "FlushDurable: no OS durability primitive available on this platform "
                  "(fflush succeeded, but the OS was never asked to persist data to "
                  "physical media)", 0});
#endif
    }

    void Close() override {
        if (fp_ != nullptr) {
            std::fclose(fp_);
            fp_ = nullptr;
        }
    }

private:
    std::FILE* fp_;
};

} // namespace

Result<std::unique_ptr<IFile>> OpenFile(const std::wstring& path, OpenMode mode) {
    const char* modeStr = nullptr;
    switch (mode) {
        case OpenMode::OpenExisting: modeStr = "r+b"; break;
        case OpenMode::CreateNew: modeStr = "w+bx"; break;
        case OpenMode::OpenOrCreate: modeStr = "a+b"; break;
    }

    std::string narrowPath = NarrowPath(path);
    std::FILE* fp = std::fopen(narrowPath.c_str(), modeStr);
    if (fp == nullptr && mode == OpenMode::OpenOrCreate) {
        fp = std::fopen(narrowPath.c_str(), "w+b");
    }
    if (fp == nullptr) {
        return Result<std::unique_ptr<IFile>>::Failure(
            Error{ErrorCode::IoError, "fopen failed for " + narrowPath, 0});
    }

    // OpenOrCreate via "a+b" positions at end-of-file for writes but we want
    // read/write from the start for our use cases; reopen in r+b once it
    // exists to get standard seek semantics.
    if (mode == OpenMode::OpenOrCreate) {
        std::fclose(fp);
        fp = std::fopen(narrowPath.c_str(), "r+b");
        if (fp == nullptr) {
            return Result<std::unique_ptr<IFile>>::Failure(
                Error{ErrorCode::IoError, "fopen (r+b) failed for " + narrowPath, 0});
        }
    }

    return Result<std::unique_ptr<IFile>>::Success(std::make_unique<PortableFile>(fp));
}

} // namespace QuantumCache::Storage
