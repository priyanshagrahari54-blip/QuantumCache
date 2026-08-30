#pragma once
#include "QuantumCache/Common/Result.h"
#include <string>

namespace QuantumCache::Service {

// Real (not stubbed) wrapper around OpenSCManagerW / CreateServiceW /
// DeleteService / OpenServiceW, i.e. the install/uninstall half of
// running QuantumCache as a Windows Service, distinct from IServiceHost
// (which handles a running service's SCM lifecycle callbacks). Splitting
// these matches how they're actually used: an elevated installer process
// calls Install()/Uninstall() once, while the service binary itself only
// ever calls IServiceHost::Run().
//
// VERIFIED: compiles/links via MinGW-w64 against advapi32. NOT VERIFIED:
// actual registration with a live SCM (requires Administrator privileges
// on real Windows, which this sandbox cannot provide).
struct ServiceInstallOptions {
    std::wstring serviceName;
    std::wstring displayName;
    std::wstring description;
    std::wstring binaryPath; // full path to the service .exe
    // SERVICE_AUTO_START vs SERVICE_DEMAND_START, expressed as a bool to
    // avoid leaking a raw Win32 DWORD through this header.
    bool autoStart{true};
};

[[nodiscard]] Common::Result<void> InstallService(const ServiceInstallOptions& options);
[[nodiscard]] Common::Result<void> UninstallService(const std::wstring& serviceName);

} // namespace QuantumCache::Service
