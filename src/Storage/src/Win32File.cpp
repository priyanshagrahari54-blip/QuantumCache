// Real Win32 implementation of IFile, built on CreateFileW / WriteFile /
// ReadFile / FlushFileBuffers / SetFilePointerEx. This file is only
// compiled when targeting Windows (WIN32 defined), whether via real MSVC
// or the MinGW-w64 cross-compiler used to verify this code in the current
// development sandbox.
//
// Verified in this environment: compiles and links into a PE32+ object
// using x86_64-w64-mingw32-g++. NOT verified: actual runtime behavior on a
// live Windows/NTFS volume. That requires testing on real Windows.
#include "QuantumCache/Storage/IFile.h"
#include <windows.h>

namespace QuantumCache::Storage {
namespace {

using Common::Error;
using Common::ErrorCode;
using Common::Result;

Error MakeWin32Error(const char* context) {
    Error err;
    err.code = ErrorCode::Win32ApiFailure;
    err.platformErrorValue = ::GetLastError();
    err.message = context;
    return err;
}

class Win32File final : public IFile {
public:
    explicit Win32File(HANDLE handle) : handle_(handle) {}

    ~Win32File() override { Close(); }

    Win32File(const Win32File&) = delete;
    Win32File& operator=(const Win32File&) = delete;

    Result<std::size_t> Read(void* buffer, std::size_t bytes) override {
        DWORD bytesRead = 0;
        BOOL ok = ::ReadFile(handle_, buffer, static_cast<DWORD>(bytes), &bytesRead, nullptr);
        if (!ok) {
            return Result<std::size_t>::Failure(MakeWin32Error("ReadFile failed"));
        }
        return Result<std::size_t>::Success(static_cast<std::size_t>(bytesRead));
    }

    Result<std::size_t> Write(const void* buffer, std::size_t bytes) override {
        DWORD bytesWritten = 0;
        BOOL ok = ::WriteFile(handle_, buffer, static_cast<DWORD>(bytes), &bytesWritten, nullptr);
        if (!ok) {
            return Result<std::size_t>::Failure(MakeWin32Error("WriteFile failed"));
        }
        return Result<std::size_t>::Success(static_cast<std::size_t>(bytesWritten));
    }

    Result<std::uint64_t> Seek(std::int64_t offset, bool fromEnd) override {
        LARGE_INTEGER distance{};
        distance.QuadPart = offset;
        LARGE_INTEGER newPos{};
        BOOL ok = ::SetFilePointerEx(
            handle_, distance, &newPos, fromEnd ? FILE_END : FILE_BEGIN);
        if (!ok) {
            return Result<std::uint64_t>::Failure(MakeWin32Error("SetFilePointerEx failed"));
        }
        return Result<std::uint64_t>::Success(static_cast<std::uint64_t>(newPos.QuadPart));
    }

    Result<std::uint64_t> Size() const override {
        LARGE_INTEGER size{};
        BOOL ok = ::GetFileSizeEx(handle_, &size);
        if (!ok) {
            return Result<std::uint64_t>::Failure(MakeWin32Error("GetFileSizeEx failed"));
        }
        return Result<std::uint64_t>::Success(static_cast<std::uint64_t>(size.QuadPart));
    }

    Result<void> SetLength(std::uint64_t length) override {
        LARGE_INTEGER distance{};
        distance.QuadPart = static_cast<LONGLONG>(length);
        LARGE_INTEGER newPos{};
        if (!::SetFilePointerEx(handle_, distance, &newPos, FILE_BEGIN)) {
            return Result<void>::Failure(MakeWin32Error("SetFilePointerEx (SetLength) failed"));
        }
        if (!::SetEndOfFile(handle_)) {
            return Result<void>::Failure(MakeWin32Error("SetEndOfFile failed"));
        }
        return Result<void>::Success();
    }

    Result<void> FlushDurable() override {
        // FlushFileBuffers forces the OS + drive cache to persist data to
        // stable media. This is the load-bearing call for crash
        // consistency: without it, a power cut can lose writes that
        // WriteFile() already reported as successful.
        BOOL ok = ::FlushFileBuffers(handle_);
        if (!ok) {
            return Result<void>::Failure(MakeWin32Error("FlushFileBuffers failed"));
        }
        return Result<void>::Success();
    }

    void Close() override {
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE handle_;
};

} // namespace

Result<std::unique_ptr<IFile>> OpenFile(const std::wstring& path, OpenMode mode) {
    DWORD creation = OPEN_EXISTING;
    switch (mode) {
        case OpenMode::OpenExisting: creation = OPEN_EXISTING; break;
        case OpenMode::CreateNew: creation = CREATE_NEW; break;
        case OpenMode::OpenOrCreate: creation = OPEN_ALWAYS; break;
    }

    HANDLE handle = ::CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        creation,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (handle == INVALID_HANDLE_VALUE) {
        return Result<std::unique_ptr<IFile>>::Failure(MakeWin32Error("CreateFileW failed"));
    }

    return Result<std::unique_ptr<IFile>>::Success(std::make_unique<Win32File>(handle));
}

} // namespace QuantumCache::Storage
