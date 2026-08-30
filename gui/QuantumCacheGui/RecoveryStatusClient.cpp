#include "pch.h"
#include "RecoveryStatusClient.h"
#include <cstring>
#include <vector>

namespace QuantumCacheGui {
namespace {

// AUDITED BUG (fixed): these used to be independently-declared local
// constants ("must match QuantumCache::Ipc::MessageType values") that drifted
// out of sync with the real protocol (kProtocolVersion here was left at 1
// after the backend moved to 2). Now aliased directly from the single
// authoritative definition in Protocol.h — there is no second place for
// these values to be declared, so they cannot drift again.
constexpr std::uint32_t kGetStatusRequest = static_cast<std::uint32_t>(QuantumCache::Ipc::MessageType::GetStatusRequest);
constexpr std::uint32_t kGetStatusResponse = static_cast<std::uint32_t>(QuantumCache::Ipc::MessageType::GetStatusResponse);
constexpr std::uint32_t kProtocolVersion = QuantumCache::Ipc::kProtocolVersion;

} // namespace

RecoveryStatusClient::RecoveryStatusClient(std::wstring pipeName) : pipeName_(std::move(pipeName)) {}

std::optional<RecoveryStatus> RecoveryStatusClient::FetchStatus() {
    HANDLE pipe = ::CreateFileW(
        pipeName_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        // Most commonly: the service is not running. Not an exceptional
        // condition the GUI should crash on.
        return std::nullopt;
    }

    DWORD mode = PIPE_READMODE_BYTE;
    ::SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    // Encode GetStatusRequest: uint32 length, uint32 type, uint32 version.
    std::uint8_t request[12];
    std::uint32_t length = sizeof(request);
    std::memcpy(request + 0, &length, 4);
    std::memcpy(request + 4, &kGetStatusRequest, 4);
    std::memcpy(request + 8, &kProtocolVersion, 4);

    DWORD written = 0;
    if (!::WriteFile(pipe, request, sizeof(request), &written, nullptr) || written != sizeof(request)) {
        ::CloseHandle(pipe);
        return std::nullopt;
    }

    std::uint32_t responseLength = 0;
    DWORD readBytes = 0;
    if (!::ReadFile(pipe, &responseLength, sizeof(responseLength), &readBytes, nullptr) ||
        readBytes != sizeof(responseLength)) {
        ::CloseHandle(pipe);
        return std::nullopt;
    }

    if (responseLength < 12 || responseLength > 4096) {
        ::CloseHandle(pipe);
        return std::nullopt;
    }

    std::vector<std::uint8_t> buffer(responseLength);
    std::memcpy(buffer.data(), &responseLength, sizeof(responseLength));

    DWORD remaining = responseLength - sizeof(responseLength);
    DWORD gotTotal = 0;
    while (gotTotal < remaining) {
        DWORD got = 0;
        if (!::ReadFile(pipe, buffer.data() + sizeof(responseLength) + gotTotal, remaining - gotTotal,
                         &got, nullptr) ||
            got == 0) {
            ::CloseHandle(pipe);
            return std::nullopt;
        }
        gotTotal += got;
    }

    ::CloseHandle(pipe);

    std::uint32_t type = 0, version = 0;
    std::memcpy(&type, buffer.data() + 4, 4);
    std::memcpy(&version, buffer.data() + 8, 4);

    if (type != kGetStatusResponse || version != kProtocolVersion) {
        return std::nullopt;
    }
    if (buffer.size() < 12 + 4 + 8 + 1) {
        return std::nullopt;
    }

    RecoveryStatus status;
    std::memcpy(&status.recoveryState, buffer.data() + 12, 4);
    std::memcpy(&status.sessionGeneration, buffer.data() + 16, 8);
    std::uint8_t activeByte = buffer[24];
    status.cacheEngineActive = (activeByte != 0);

    return status;
}

} // namespace QuantumCacheGui
