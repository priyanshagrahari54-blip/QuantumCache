#pragma once
#include "QuantumCache/Common/Result.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace QuantumCache::Ipc {

// Transport abstraction over a single local named-pipe connection carrying
// length-prefixed frames produced/consumed by MessageCodec. Split from
// MessageCodec so wire-format logic can be tested without any OS pipe
// dependency, while the transport itself has a real Win32 implementation.
class INamedPipeTransport {
public:
    virtual ~INamedPipeTransport() = default;

    [[nodiscard]] virtual Common::Result<void> SendFrame(const std::vector<std::uint8_t>& frame) = 0;
    [[nodiscard]] virtual Common::Result<std::vector<std::uint8_t>> ReceiveFrame() = 0;
    virtual void Close() = 0;
};

// Server-side: creates (Win32 CreateNamedPipeW) and waits for exactly one
// client connection, then returns a transport bound to that connection.
// The Windows Service uses this once per GUI client session.
class INamedPipeServer {
public:
    virtual ~INamedPipeServer() = default;

    // AUDITED BUG (fixed): AcceptOnce() previously used a purely
    // synchronous, non-cancellable ConnectNamedPipe() call, which meant
    // the only way to make a blocked accept-loop thread return during
    // shutdown was to leak/detach it — the thread could then use
    // already-destroyed objects if it later woke up (e.g. a delayed
    // client connection arriving after the "owning" objects had been
    // torn down), and the process could not exit promptly if nothing
    // ever connected. AcceptOnce() is now built on real Win32 overlapped
    // I/O, so it can return promptly and deterministically when
    // RequestShutdown() is called from another thread, even with zero
    // pending client connections. Returns ErrorCode::ServiceStopping
    // (not a generic Win32 failure) when it was unblocked specifically
    // because of a shutdown request, so callers can distinguish "asked
    // to stop" from "a real connection error occurred."
    [[nodiscard]] virtual Common::Result<std::unique_ptr<INamedPipeTransport>> AcceptOnce() = 0;

    // Unblocks any in-progress or future AcceptOnce() call, causing it to
    // return promptly with ErrorCode::ServiceStopping instead of blocking
    // indefinitely for a client that may never connect. Safe to call from
    // a different thread than the one calling AcceptOnce() (that is its
    // entire purpose), and safe to call more than once. After this is
    // called, AcceptOnce() will always return immediately without
    // attempting a new accept.
    virtual void RequestShutdown() = 0;
};

[[nodiscard]] Common::Result<std::unique_ptr<INamedPipeServer>> CreateNamedPipeServer(
    const std::wstring& pipeName);

[[nodiscard]] Common::Result<std::unique_ptr<INamedPipeTransport>> ConnectNamedPipeClient(
    const std::wstring& pipeName);

} // namespace QuantumCache::Ipc
