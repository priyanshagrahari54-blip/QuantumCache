#pragma once
#include "QuantumCache/Common/Result.h"
#include <functional>
#include <memory>
#include <string>

namespace QuantumCache::Service {

// Lifecycle callbacks the Windows Service Control Manager (SCM) drives.
// This interface exists so the actual SCM plumbing (Win32-only) can be
// tested/exercised independently of the policy code that decides what
// happens on start/stop — in particular, so PowerResilience's
// MarkCleanShutdown() is reachable from every orderly-stop path.
struct ServiceCallbacks {
    // Called once after StartServiceCtrlDispatcher hands control to the
    // service's ServiceMain. Must perform IRecoveryManager::InitializeAndRecover
    // before signaling SERVICE_RUNNING, per the power-resilience design:
    // the service must not report itself as running while recovery state
    // is unresolved.
    std::function<Common::Result<void>()> onStart;

    // Called for SERVICE_CONTROL_STOP and SERVICE_CONTROL_SHUTDOWN. Must
    // call IRecoveryManager::MarkCleanShutdown() before returning, so the
    // NEXT startup correctly sees a clean shutdown rather than assuming a
    // power loss occurred.
    std::function<Common::Result<void>()> onStop;

    // Called for SERVICE_CONTROL_PRESHUTDOWN, which Windows sends before
    // SERVICE_CONTROL_SHUTDOWN with more time budget — useful for a cache
    // service that may need to flush more data than a bare SHUTDOWN
    // handler's tight time budget allows. Distinguished from onStop
    // because, once the real deferred-write engine exists, its "flush
    // pending writes" work belongs here, not in the previous two.
    std::function<Common::Result<void>()> onPreShutdown;
};

// Wraps the Win32 Service Control Manager integration
// (StartServiceCtrlDispatcher / RegisterServiceCtrlHandlerEx /
// SetServiceStatus). Stage 1 provides a REAL implementation of this
// plumbing (it compiles and links against winsvc.h under MinGW-w64), but
// it has not been installed/started/stopped against a live Windows SCM in
// this sandbox — that requires an actual Windows machine.
class IServiceHost {
public:
    virtual ~IServiceHost() = default;

    // Blocks for the lifetime of the service process (mirrors
    // StartServiceCtrlDispatcher's blocking behavior). Returns once the
    // SCM has told the process to stop and onStop/onPreShutdown have run.
    [[nodiscard]] virtual Common::Result<void> Run(const ServiceCallbacks& callbacks) = 0;
};

[[nodiscard]] Common::Result<std::unique_ptr<IServiceHost>> CreateServiceHost(
    const std::wstring& serviceName);

} // namespace QuantumCache::Service
