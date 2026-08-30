#pragma once
// Thin, real IPC client the GUI uses to ask the running QuantumCacheService
// for its current PowerResilience::RecoveryState over the named pipe
// protocol defined in src/Ipc/include/QuantumCache/Ipc/Protocol.h.
//
// By design (see docs/ENVIRONMENT.md "Why the GUI is a separate build
// system from the rest"), this MSBuild/WinUI3 project does not statically
// link the CMake-built QuantumCache::Ipc library — but it DOES directly
// #include the plain-data Protocol.h header (QuantumCache::Ipc::MessageType,
// QuantumCache::Ipc::kProtocolVersion, and the payload structs), since that
// header has zero platform/library dependency (only <cstdint>/<string>) and
// can be shared as source without pulling in the whole Ipc static library
// or CMake build. This is a deliberate architectural fix for an AUDITED BUG:
// earlier code in this file re-declared MessageType numeric values AND the
// protocol version as independent local constants ("must be kept in sync"),
// and that promise was not kept — the backend's protocol version was bumped
// from 1 to 2 in Stage 2 (see Protocol.h) while this file's local
// kProtocolVersion constant silently stayed at 1, so a real GUI build would
// have had every request rejected by MessageCodec's version check. Directly
// including Protocol.h makes that entire class of bug structurally
// impossible: there is exactly one authoritative definition of the protocol
// version and message-type values (see also
// tests/IpcCodecTests.cpp's ProtocolVersionMismatch_IsDetected test and the
// new GuiClientProtocolVersion_MatchesAuthoritativeDefinition regression
// test, which fail to compile/link if this ever duplicates the constant
// again instead of including the header).
//
// STATUS: real, working-as-written code; NOT compiled or run in this
// sandbox (no C++/WinRT/Windows App SDK toolchain available here). Must
// be validated against a live QuantumCacheService.exe on real Windows.
#include <windows.h>
#include <cstdint>
#include <optional>
#include <string>

// NOTE: this relative include path assumes the GUI project's include
// directories are configured to see the backend's public headers (see
// QuantumCacheGui.vcxproj's AdditionalIncludeDirectories, which adds
// ../../src/Ipc/include specifically for this purpose). Protocol.h has no
// Win32/WinRT/CMake-target dependency, so this is a safe, header-only,
// source-level share — the GUI still does not link against the Ipc
// static library or any other backend build output.
#include "QuantumCache/Ipc/Protocol.h"

namespace QuantumCacheGui {

struct RecoveryStatus {
    std::uint32_t recoveryState{0};   // mirrors QuantumCache::PowerResilience::RecoveryState
    std::uint64_t sessionGeneration{0};
    bool cacheEngineActive{false};
};

class RecoveryStatusClient {
public:
    explicit RecoveryStatusClient(std::wstring pipeName);

    // Connects, sends GetStatusRequest, reads GetStatusResponse, and
    // disconnects. Returns std::nullopt on any failure (pipe not present
    // because the service isn't running, protocol mismatch, etc.) — the
    // GUI must treat that as "status unknown", never fabricate a status.
    [[nodiscard]] std::optional<RecoveryStatus> FetchStatus();

private:
    std::wstring pipeName_;
};

} // namespace QuantumCacheGui
