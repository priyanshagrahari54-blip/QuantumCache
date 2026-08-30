#include "QuantumCache/Service/ServiceInstaller.h"
#include <windows.h>

namespace QuantumCache::Service {
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

class ScHandleGuard {
public:
    explicit ScHandleGuard(SC_HANDLE h) : handle_(h) {}
    ~ScHandleGuard() { if (handle_) ::CloseServiceHandle(handle_); }
    ScHandleGuard(const ScHandleGuard&) = delete;
    ScHandleGuard& operator=(const ScHandleGuard&) = delete;
    [[nodiscard]] SC_HANDLE get() const noexcept { return handle_; }
private:
    SC_HANDLE handle_;
};

} // namespace

Result<void> InstallService(const ServiceInstallOptions& options) {
    if (options.serviceName.empty() || options.binaryPath.empty()) {
        return Result<void>::Failure(
            Error{ErrorCode::InvalidArgument, "serviceName and binaryPath must not be empty", 0});
    }

    ScHandleGuard scm(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE));
    if (scm.get() == nullptr) {
        return Result<void>::Failure(MakeWin32Error(
            "OpenSCManagerW failed (requires Administrator privileges on real Windows)"));
    }

    DWORD startType = options.autoStart ? SERVICE_AUTO_START : SERVICE_DEMAND_START;

    ScHandleGuard service(::CreateServiceW(
        scm.get(),
        options.serviceName.c_str(),
        options.displayName.empty() ? options.serviceName.c_str() : options.displayName.c_str(),
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        startType,
        SERVICE_ERROR_NORMAL,
        options.binaryPath.c_str(),
        nullptr, nullptr, nullptr, nullptr, nullptr));

    if (service.get() == nullptr) {
        return Result<void>::Failure(MakeWin32Error("CreateServiceW failed"));
    }

    if (!options.description.empty()) {
        SERVICE_DESCRIPTIONW desc{};
        desc.lpDescription = const_cast<LPWSTR>(options.description.c_str());
        // Non-fatal if this fails; the service is still installed.
        ::ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_DESCRIPTION, &desc);
    }

    // Configure delayed auto-start is intentionally NOT set here in Stage
    // 1 — that is a product/performance decision belonging with the real
    // engine, not the install-time plumbing.

    return Result<void>::Success();
}

Result<void> UninstallService(const std::wstring& serviceName) {
    if (serviceName.empty()) {
        return Result<void>::Failure(
            Error{ErrorCode::InvalidArgument, "serviceName must not be empty", 0});
    }

    ScHandleGuard scm(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (scm.get() == nullptr) {
        return Result<void>::Failure(MakeWin32Error(
            "OpenSCManagerW failed (requires Administrator privileges on real Windows)"));
    }

    ScHandleGuard service(::OpenServiceW(scm.get(), serviceName.c_str(), DELETE | SERVICE_STOP));
    if (service.get() == nullptr) {
        return Result<void>::Failure(MakeWin32Error("OpenServiceW failed"));
    }

    if (!::DeleteService(service.get())) {
        return Result<void>::Failure(MakeWin32Error("DeleteService failed"));
    }

    return Result<void>::Success();
}

} // namespace QuantumCache::Service
