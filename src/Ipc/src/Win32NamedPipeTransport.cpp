// Real Win32 named-pipe transport using CreateNamedPipeW / ConnectNamedPipe
// / CreateFileW / ReadFile / WriteFile. Compiled only when targeting
// Windows. Verified in this sandbox only to the extent it compiles/links
// via MinGW-w64 into a PE object — actual inter-process named pipe
// behavior (including the security descriptor / ACL needed to let a
// non-elevated GUI talk to a LocalSystem service) has NOT been exercised
// and must be validated on real Windows. The security descriptor for the
// pipe deliberately uses a restrictive, explicit DACL rather than the
// default (NULL) security descriptor, since this pipe crosses a privilege
// boundary between a LocalSystem service and a per-user GUI process.
#include "QuantumCache/Ipc/INamedPipeTransport.h"
#include <windows.h>
#include <sddl.h>
#include <cstring>

namespace QuantumCache::Ipc {
namespace {

using Common::Error;
using Common::ErrorCode;
using Common::Result;

constexpr DWORD kPipeBufferSize = 8192;
constexpr DWORD kPipeTimeoutMs = 5000;

Error MakeWin32Error(const char* context) {
    Error err;
    err.code = ErrorCode::Win32ApiFailure;
    err.platformErrorValue = ::GetLastError();
    err.message = context;
    return err;
}

class Win32NamedPipeTransport final : public INamedPipeTransport {
public:
    explicit Win32NamedPipeTransport(HANDLE handle) : handle_(handle) {}
    ~Win32NamedPipeTransport() override { Close(); }

    Result<void> SendFrame(const std::vector<std::uint8_t>& frame) override {
        DWORD written = 0;
        BOOL ok = ::WriteFile(handle_, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr);
        if (!ok || written != frame.size()) {
            return Result<void>::Failure(MakeWin32Error("WriteFile on named pipe failed"));
        }
        return Result<void>::Success();
    }

    Result<std::vector<std::uint8_t>> ReceiveFrame() override {
        // Frame format: uint32_t totalLength (includes itself), then the
        // remaining bytes. Read the length first, then the rest.
        std::uint32_t totalLength = 0;
        DWORD readBytes = 0;
        BOOL ok = ::ReadFile(handle_, &totalLength, sizeof(totalLength), &readBytes, nullptr);
        if (!ok || readBytes != sizeof(totalLength)) {
            return Result<std::vector<std::uint8_t>>::Failure(
                MakeWin32Error("ReadFile (length prefix) on named pipe failed"));
        }
        if (totalLength < sizeof(totalLength) || totalLength > (16u * 1024u * 1024u)) {
            return Result<std::vector<std::uint8_t>>::Failure(
                Error{ErrorCode::CorruptData, "IPC frame length out of sane bounds", 0});
        }

        std::vector<std::uint8_t> frame(totalLength);
        std::memcpy(frame.data(), &totalLength, sizeof(totalLength));

        DWORD remaining = static_cast<DWORD>(totalLength - sizeof(totalLength));
        DWORD gotTotal = 0;
        while (gotTotal < remaining) {
            DWORD got = 0;
            ok = ::ReadFile(handle_, frame.data() + sizeof(totalLength) + gotTotal,
                             remaining - gotTotal, &got, nullptr);
            if (!ok || got == 0) {
                return Result<std::vector<std::uint8_t>>::Failure(
                    MakeWin32Error("ReadFile (payload) on named pipe failed"));
            }
            gotTotal += got;
        }

        return Result<std::vector<std::uint8_t>>::Success(std::move(frame));
    }

    void Close() override {
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
            ::FlushFileBuffers(handle_);
            ::DisconnectNamedPipe(handle_);
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE handle_;
};

class Win32NamedPipeServer final : public INamedPipeServer {
public:
    explicit Win32NamedPipeServer(std::wstring pipeName) : pipeName_(std::move(pipeName)) {
        // Manual-reset event used purely as the "please stop" signal for
        // WaitForMultipleObjects below; never itself represents I/O
        // completion. Created once for the server's lifetime, not
        // per-AcceptOnce-call, so RequestShutdown() can be called safely
        // before, during, or after any given AcceptOnce() invocation.
        stopEvent_ = ::CreateEventW(nullptr, /*manualReset=*/TRUE, /*initialState=*/FALSE, nullptr);
    }

    ~Win32NamedPipeServer() override {
        if (stopEvent_ != nullptr) {
            ::CloseHandle(stopEvent_);
        }
    }

    // AUDITED BUG (fixed): AcceptOnce() previously called the synchronous
    // ConnectNamedPipe(handle, nullptr), which cannot be interrupted from
    // another thread by any documented Win32 mechanism. The service's
    // shutdown path used to work around this by DETACHING the accept
    // thread rather than joining it — meaning the thread could still be
    // running (and could still be about to touch shared state) after
    // OnServiceStop() returned and the service reported itself fully
    // stopped. This implementation now opens the pipe with
    // FILE_FLAG_OVERLAPPED and issues an overlapped ConnectNamedPipe,
    // then waits on BOTH the connection's completion event AND
    // stopEvent_ via WaitForMultipleObjects — whichever is signaled
    // first wins. If shutdown wins the race, the half-open pipe instance
    // is cleanly cancelled (CancelIoEx) and closed before returning, so
    // no handle or thread is ever leaked or left detached.
    Result<std::unique_ptr<INamedPipeTransport>> AcceptOnce() override {
        if (::WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) {
            return Result<std::unique_ptr<INamedPipeTransport>>::Failure(
                Error{ErrorCode::ServiceStopping, "AcceptOnce: shutdown already requested", 0});
        }

        // Explicit DACL: allow the built-in "Authenticated Users" group
        // read/write access, but not "Everyone" / anonymous. This is
        // deliberately more restrictive than the Win32 default (which
        // would otherwise grant broad access), because this pipe connects
        // a LocalSystem service to a per-user GUI. Real validation of this
        // ACL (e.g. with a low-privilege test client) can only happen on
        // real Windows. See docs/STAGE2_ARCHITECTURE.md "Named-pipe
        // security" for the full threat-model rationale and why this
        // specific SDDL string ("Authenticated Users": generic read+write,
        // no broader grant) was chosen over both the Win32 default
        // security descriptor (NULL DACL, effectively "Everyone") and an
        // even narrower single-SID descriptor that would prevent a
        // legitimate secondary user session from ever connecting.
        PSECURITY_DESCRIPTOR sd = nullptr;
        const wchar_t* sddl = L"D:(A;;GRGW;;;AU)"; // Allow Authenticated Users Generic Read/Write
        if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl, SDDL_REVISION_1, &sd, nullptr)) {
            return Result<std::unique_ptr<INamedPipeTransport>>::Failure(
                MakeWin32Error("ConvertStringSecurityDescriptorToSecurityDescriptorW failed"));
        }

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = sd;
        sa.bInheritHandle = FALSE;

        HANDLE handle = ::CreateNamedPipeW(
            pipeName_.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, // single instance: Stage 1 supports exactly one GUI session at a time
            kPipeBufferSize,
            kPipeBufferSize,
            kPipeTimeoutMs,
            &sa);

        ::LocalFree(sd);

        if (handle == INVALID_HANDLE_VALUE) {
            return Result<std::unique_ptr<INamedPipeTransport>>::Failure(
                MakeWin32Error("CreateNamedPipeW failed"));
        }

        HANDLE connectEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (connectEvent == nullptr) {
            auto err = MakeWin32Error("CreateEventW (connect) failed");
            ::CloseHandle(handle);
            return Result<std::unique_ptr<INamedPipeTransport>>::Failure(err);
        }

        OVERLAPPED overlapped{};
        overlapped.hEvent = connectEvent;

        BOOL connected = ::ConnectNamedPipe(handle, &overlapped);
        DWORD connectError = ::GetLastError();

        if (!connected && connectError == ERROR_IO_PENDING) {
            // The common, expected case: no client was already waiting,
            // so the connect is genuinely asynchronous. Wait for EITHER
            // it to complete OR a shutdown request — this is the actual
            // fix: previously there was no way to wait on "either of
            // these two things", only a blocking wait on the connect
            // alone.
            HANDLE waitHandles[2] = {connectEvent, stopEvent_};
            DWORD waitResult = ::WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

            if (waitResult == WAIT_OBJECT_0 + 1) {
                // Shutdown was requested while waiting for a client.
                // Cancel the pending overlapped ConnectNamedPipe cleanly
                // before tearing down the handle, so Windows does not
                // consider it abandoned mid-operation.
                ::CancelIoEx(handle, &overlapped);
                DWORD unusedBytes = 0;
                // GetOverlappedResult with bWait=TRUE drains the
                // cancellation completion so the OVERLAPPED structure
                // (stack-allocated here) is not still "in flight" from
                // the kernel's point of view when this function returns.
                ::GetOverlappedResult(handle, &overlapped, &unusedBytes, TRUE);
                ::CloseHandle(connectEvent);
                ::CloseHandle(handle);
                return Result<std::unique_ptr<INamedPipeTransport>>::Failure(
                    Error{ErrorCode::ServiceStopping, "AcceptOnce: shutdown requested while waiting for a client", 0});
            }

            if (waitResult != WAIT_OBJECT_0) {
                DWORD waitErr = ::GetLastError();
                ::CancelIoEx(handle, &overlapped);
                ::CloseHandle(connectEvent);
                ::CloseHandle(handle);
                Error err;
                err.code = ErrorCode::Win32ApiFailure;
                err.platformErrorValue = waitErr;
                err.message = "AcceptOnce: WaitForMultipleObjects failed";
                return Result<std::unique_ptr<INamedPipeTransport>>::Failure(err);
            }

            // Connection completed; confirm success and retrieve the
            // real result (mirrors what a synchronous ConnectNamedPipe
            // would have reported).
            DWORD transferredBytes = 0;
            if (!::GetOverlappedResult(handle, &overlapped, &transferredBytes, FALSE)) {
                DWORD resultErr = ::GetLastError();
                ::CloseHandle(connectEvent);
                ::CloseHandle(handle);
                Error err;
                err.code = ErrorCode::Win32ApiFailure;
                err.platformErrorValue = resultErr;
                err.message = "AcceptOnce: GetOverlappedResult (connect) failed";
                return Result<std::unique_ptr<INamedPipeTransport>>::Failure(err);
            }
        } else if (!connected && connectError != ERROR_PIPE_CONNECTED) {
            // A real, immediate failure (not "pending", not "a client
            // was already there when we called ConnectNamedPipe").
            ::CloseHandle(connectEvent);
            ::CloseHandle(handle);
            Error err;
            err.code = ErrorCode::Win32ApiFailure;
            err.platformErrorValue = connectError;
            err.message = "ConnectNamedPipe failed";
            return Result<std::unique_ptr<INamedPipeTransport>>::Failure(err);
        }
        // else: connected == TRUE, or connectError == ERROR_PIPE_CONNECTED
        // (a client was already waiting) — either way the pipe is
        // connected synchronously and connectEvent was never actually
        // needed, but is still closed below for symmetry.

        ::CloseHandle(connectEvent);

        return Result<std::unique_ptr<INamedPipeTransport>>::Success(
            std::make_unique<Win32NamedPipeTransport>(handle));
    }

    void RequestShutdown() override {
        if (stopEvent_ != nullptr) {
            ::SetEvent(stopEvent_);
        }
    }

private:
    std::wstring pipeName_;
    HANDLE stopEvent_{nullptr};
};

} // namespace

Result<std::unique_ptr<INamedPipeServer>> CreateNamedPipeServer(const std::wstring& pipeName) {
    if (pipeName.empty()) {
        return Result<std::unique_ptr<INamedPipeServer>>::Failure(
            Error{ErrorCode::InvalidArgument, "pipeName must not be empty", 0});
    }
    return Result<std::unique_ptr<INamedPipeServer>>::Success(
        std::make_unique<Win32NamedPipeServer>(pipeName));
}

Result<std::unique_ptr<INamedPipeTransport>> ConnectNamedPipeClient(const std::wstring& pipeName) {
    HANDLE handle = ::CreateFileW(
        pipeName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (handle == INVALID_HANDLE_VALUE) {
        return Result<std::unique_ptr<INamedPipeTransport>>::Failure(
            MakeWin32Error("CreateFileW (named pipe client) failed"));
    }

    DWORD mode = PIPE_READMODE_BYTE;
    ::SetNamedPipeHandleState(handle, &mode, nullptr, nullptr);

    return Result<std::unique_ptr<INamedPipeTransport>>::Success(
        std::make_unique<Win32NamedPipeTransport>(handle));
}

} // namespace QuantumCache::Ipc
