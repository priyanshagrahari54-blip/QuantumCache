// Real Win32 Windows Service implementation using StartServiceCtrlDispatcherW,
// RegisterServiceCtrlHandlerExW, and SetServiceStatus. Compiled only for
// Windows targets.
//
// VERIFIED in this sandbox: compiles and links cleanly against winsvc.h /
// advapi32 under x86_64-w64-mingw32-g++, producing a real PE object.
// NOT VERIFIED: this code has never been installed as an actual Windows
// service or driven by a live Service Control Manager, because no Windows
// kernel is available in this development sandbox. SERVICE_CONTROL_STOP /
// SHUTDOWN / PRESHUTDOWN handling, checkpointing during slow shutdowns,
// and interaction with `sc.exe` / services.msc must be validated on real
// Windows before this is trusted in production.
#include "QuantumCache/Service/IServiceHost.h"
#include <windows.h>
#include <atomic>
#include <cwchar>

namespace QuantumCache::Service {
namespace {

using Common::Error;
using Common::ErrorCode;
using Common::Result;

// The Win32 Service Control Handler API is a set of process-wide C
// callbacks with no user-data parameter in the classic
// RegisterServiceCtrlHandler; RegisterServiceCtrlHandlerExW does pass a
// context pointer, which we use to reach back into our C++ object,
// avoiding any global mutable service instance beyond what Win32 itself
// requires.
class Win32ServiceHost final : public IServiceHost {
public:
    explicit Win32ServiceHost(std::wstring serviceName) : serviceName_(std::move(serviceName)) {}

    Result<void> Run(const ServiceCallbacks& callbacks) override {
        callbacks_ = callbacks;

        SERVICE_TABLE_ENTRYW table[] = {
            {const_cast<LPWSTR>(serviceName_.c_str()), &Win32ServiceHost::ServiceMainThunk},
            {nullptr, nullptr}
        };

        // StartServiceCtrlDispatcherW requires this static instance
        // pointer because ServiceMain's signature is fixed by Win32 and
        // carries no user context.
        activeInstance_ = this;

        BOOL ok = ::StartServiceCtrlDispatcherW(table);
        activeInstance_ = nullptr;

        if (!ok) {
            Error err;
            err.code = ErrorCode::Win32ApiFailure;
            err.platformErrorValue = ::GetLastError();
            err.message = "StartServiceCtrlDispatcherW failed "
                           "(note: this call only succeeds when the process was actually "
                           "launched by the Service Control Manager, not when run interactively)";
            return Result<void>::Failure(err);
        }

        return runResult_;
    }

private:
    static void WINAPI ServiceMainThunk(DWORD argc, LPWSTR* argv) {
        if (activeInstance_ != nullptr) {
            activeInstance_->ServiceMain(argc, argv);
        }
    }

    static DWORD WINAPI ControlHandlerThunk(DWORD control, DWORD, LPVOID, LPVOID context) {
        auto* self = reinterpret_cast<Win32ServiceHost*>(context);
        if (self != nullptr) {
            return self->HandleControl(control);
        }
        return NO_ERROR;
    }

    void ServiceMain(DWORD, LPWSTR*) {
        statusHandle_ = ::RegisterServiceCtrlHandlerExW(
            serviceName_.c_str(), &Win32ServiceHost::ControlHandlerThunk, this);

        if (statusHandle_ == nullptr) {
            runResult_ = Result<void>::Failure(
                Error{ErrorCode::Win32ApiFailure, "RegisterServiceCtrlHandlerExW failed",
                      ::GetLastError()});
            return;
        }

        ReportStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

        if (callbacks_.onStart) {
            auto startResult = callbacks_.onStart();
            if (!startResult) {
                ReportStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 0);
                runResult_ = startResult;
                return;
            }
        }

        ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);

        // Block until a stop has been requested via the control handler.
        stopEvent_.wait(false);

        ReportStatus(SERVICE_STOPPED, NO_ERROR, 0);
        runResult_ = Result<void>::Success();
    }

    DWORD HandleControl(DWORD control) {
        switch (control) {
            case SERVICE_CONTROL_INTERROGATE:
                return NO_ERROR;

            case SERVICE_CONTROL_PRESHUTDOWN: {
                ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 10000);
                if (callbacks_.onPreShutdown) {
                    callbacks_.onPreShutdown();
                }
                return NO_ERROR;
            }

            case SERVICE_CONTROL_STOP:
            case SERVICE_CONTROL_SHUTDOWN: {
                ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
                if (callbacks_.onStop) {
                    callbacks_.onStop();
                }
                stopEvent_.store(true);
                stopEvent_.notify_all();
                return NO_ERROR;
            }

            default:
                return ERROR_CALL_NOT_IMPLEMENTED;
        }
    }

    void ReportStatus(DWORD state, DWORD exitCode, DWORD waitHintMs) {
        SERVICE_STATUS status{};
        status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        status.dwCurrentState = state;
        status.dwWin32ExitCode = exitCode;
        status.dwWaitHint = waitHintMs;

        status.dwControlsAccepted =
            (state == SERVICE_START_PENDING)
                ? 0
                : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PRESHUTDOWN);

        status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED)
                                   ? 0
                                   : ++checkpoint_;

        ::SetServiceStatus(statusHandle_, &status);
    }

    std::wstring serviceName_;
    ServiceCallbacks callbacks_;
    SERVICE_STATUS_HANDLE statusHandle_{nullptr};
    std::atomic<bool> stopEvent_{false};
    DWORD checkpoint_{0};
    Result<void> runResult_{Result<void>::Success()};

    static inline Win32ServiceHost* activeInstance_{nullptr};
};

} // namespace

Result<std::unique_ptr<IServiceHost>> CreateServiceHost(const std::wstring& serviceName) {
    if (serviceName.empty()) {
        return Result<std::unique_ptr<IServiceHost>>::Failure(
            Error{ErrorCode::InvalidArgument, "serviceName must not be empty", 0});
    }
    return Result<std::unique_ptr<IServiceHost>>::Success(
        std::make_unique<Win32ServiceHost>(serviceName));
}

} // namespace QuantumCache::Service
