// Real entry point for QuantumCacheService.exe.
//
// Stage 2 startup sequence (matches the required conceptual sequence
// exactly):
//   service start
//   -> recovery initialization         (RecoveryManager::InitializeAndRecover starts)
//   -> journal validation/replay       (IWriteAheadJournal::Replay, via CacheEngine::ReplayFromJournal
//                                        invoked ONLY from inside the replay callback, i.e. only when
//                                        InitializeAndRecover actually determines replay is needed)
//   -> recovery completion             (InitializeAndRecover returns success)
//   -> cache engine initialization     (CacheEngine::MarkRecoveryComplete)
//   -> only then normal cache operations (IPC server starts accepting requests)
// SERVICE_RUNNING is never reported (IServiceHost::Run only calls
// ReportStatus(SERVICE_RUNNING, ...) after onStart returns success) until
// every one of the steps above has actually succeeded.
//
// Stage 2 shutdown sequence (matches the required conceptual sequence):
//   stop accepting unsafe new operations  (CacheEngine::Shutdown() flips
//                                           lifecycle to Stopping, so
//                                           concurrent Put/Invalidate calls
//                                           already in flight start seeing
//                                           ErrorCode::ServiceStopping)
//   -> flush according to configured safe policy (CacheEngine::Shutdown()
//                                                  internally calls FlushAll())
//   -> persist required state             (journal truncation, only if
//                                           everything flushed cleanly —
//                                           see CacheEngine::Shutdown())
//   -> mark clean shutdown                (RecoveryManager::MarkCleanShutdown)
//   -> stop service
//
// This file is Win32-only (compiled only into the QuantumCacheService.exe
// target). Verified in this sandbox to compile/link via MinGW-w64 into a
// PE executable; NOT verified against a live Windows Service Control
// Manager — see docs/ENVIRONMENT.md and docs/STAGE2_ARCHITECTURE.md.
#include "QuantumCache/Configuration/IConfigStore.h"
#include "QuantumCache/Logging/Logger.h"
#include "QuantumCache/Logging/FileLogSink.h"
#include "QuantumCache/PowerResilience/ISessionMarker.h"
#include "QuantumCache/PowerResilience/IWriteAheadJournal.h"
#include "QuantumCache/PowerResilience/IRecoveryManager.h"
#include "QuantumCache/Storage/IFile.h"
#include "QuantumCache/Storage/IBackingStore.h"
#include "QuantumCache/CoreEngine/ICacheEngine.h"
#include "QuantumCache/Ipc/INamedPipeTransport.h"
#include "QuantumCache/Ipc/MessageCodec.h"
#include "QuantumCache/Service/IServiceHost.h"
#include <windows.h>
#include <atomic>
#include <memory>
#include <thread>

namespace {

using namespace QuantumCache;

constexpr wchar_t kServiceName[] = L"QuantumCacheService";

std::shared_ptr<Logging::Logger> g_logger;
std::unique_ptr<PowerResilience::IRecoveryManager> g_recoveryManager;
std::shared_ptr<CoreEngine::ICacheEngine> g_cacheEngine;
std::unique_ptr<Ipc::INamedPipeServer> g_ipcServer;
std::atomic<bool> g_ipcShouldStop{false};
std::thread g_ipcThread;

// Maps a CoreEngine::CacheEngineOptions field from the string-based
// AppConfig fields, rejecting nothing here that JsonConfigStore::Validate
// would not already have rejected — this function assumes `config` was
// already validated (see OnServiceStart).
CoreEngine::CacheEngineOptions BuildEngineOptions(const Configuration::AppConfig& config) {
    CoreEngine::CacheEngineOptions options;
    options.enabled = config.cacheEnabled;
    options.capacityBytes = config.cacheCapacityBytes;
    options.maxEntryCount = config.cacheMaxEntryCount;
    options.shardCount = config.cacheShardCount;
    options.evictionPolicy = CoreEngine::EvictionPolicyKind::LeastRecentlyUsed; // only value Validate() accepts
    options.writePolicy = (config.writePolicy == "WriteThrough")
                               ? CoreEngine::WritePolicyKind::WriteThrough
                               : CoreEngine::WritePolicyKind::WriteBackDeferred;
    options.flushPolicy = (config.flushPolicy == "PeriodicBackground")
                               ? CoreEngine::FlushPolicyKind::PeriodicBackground
                               : CoreEngine::FlushPolicyKind::Manual;
    options.flushIntervalSeconds = config.flushIntervalSeconds;
    return options;
}

Ipc::CacheStatisticsPayload ToIpcPayload(const CoreEngine::CacheStatistics& stats) {
    Ipc::CacheStatisticsPayload payload;
    payload.hitCount = stats.hitCount;
    payload.missCount = stats.missCount;
    payload.insertCount = stats.insertCount;
    payload.updateCount = stats.updateCount;
    payload.invalidationCount = stats.invalidationCount;
    payload.evictionCount = stats.evictionCount;
    payload.flushSuccessCount = stats.flushSuccessCount;
    payload.flushFailureCount = stats.flushFailureCount;
    payload.currentEntryCount = stats.currentEntryCount;
    payload.currentMemoryBytes = stats.currentMemoryBytes;
    payload.dirtyEntryCount = stats.dirtyEntryCount;
    payload.dirtyBytes = stats.dirtyBytes;
    return payload;
}

// Handles exactly one request on an already-connected transport, then
// returns. The IPC thread loop (below) accepts one connection at a time
// (matching Win32NamedPipeServer's documented single-instance design) and
// calls this once per connection.
void HandleOneIpcRequest(Ipc::INamedPipeTransport& transport) {
    auto frameResult = transport.ReceiveFrame();
    if (!frameResult) return;

    auto typeResult = Ipc::MessageCodec::PeekMessageType(frameResult.Value());
    if (!typeResult) return;

    switch (typeResult.Value()) {
        case Ipc::MessageType::GetStatusRequest: {
            Ipc::StatusResponsePayload payload;
            payload.recoveryState = static_cast<std::uint32_t>(
                g_recoveryManager ? g_recoveryManager->CurrentState()
                                   : PowerResilience::RecoveryState::Unknown);
            payload.sessionGeneration = 0; // populated below if available
            payload.cacheEngineActive = (g_cacheEngine != nullptr) &&
                                         g_recoveryManager != nullptr &&
                                         g_recoveryManager->CurrentState() ==
                                             PowerResilience::RecoveryState::RecoveryComplete;
            auto frame = Ipc::MessageCodec::EncodeStatusResponse(payload);
            (void)transport.SendFrame(frame);
            break;
        }

        case Ipc::MessageType::GetCacheStatisticsRequest: {
            if (!g_cacheEngine) break;
            auto stats = g_cacheEngine->GetStatistics();
            auto frame = Ipc::MessageCodec::EncodeCacheStatisticsResponse(ToIpcPayload(stats));
            (void)transport.SendFrame(frame);
            break;
        }

        case Ipc::MessageType::FlushAllRequest: {
            Ipc::OperationResultPayload result;
            if (!g_cacheEngine) {
                result.succeeded = false;
                result.errorCode = static_cast<std::uint32_t>(Common::ErrorCode::RecoveryNotComplete);
                result.message = "cache engine is not available";
            } else {
                auto flushResult = g_cacheEngine->FlushAll();
                result.succeeded = flushResult.IsOk();
                result.errorCode = static_cast<std::uint32_t>(flushResult.Err().code);
                result.message = flushResult.IsOk() ? "flush complete" : flushResult.Err().message;
            }
            auto frame = Ipc::MessageCodec::EncodeOperationResult(Ipc::MessageType::FlushAllResponse, result);
            (void)transport.SendFrame(frame);
            break;
        }

        case Ipc::MessageType::InvalidateKeyRequest: {
            auto requestResult = Ipc::MessageCodec::DecodeInvalidateKeyRequest(frameResult.Value());
            Ipc::OperationResultPayload result;
            if (!requestResult) {
                result.succeeded = false;
                result.errorCode = static_cast<std::uint32_t>(requestResult.Err().code);
                result.message = requestResult.Err().message;
            } else if (!g_cacheEngine) {
                result.succeeded = false;
                result.errorCode = static_cast<std::uint32_t>(Common::ErrorCode::RecoveryNotComplete);
                result.message = "cache engine is not available";
            } else {
                // IPC only ever exposes the FORCED invalidate (see
                // Protocol.h rationale): a remote caller cannot make an
                // informed "override dirty data" decision, so the one
                // operation exposed has one unambiguous meaning.
                auto invalidateResult = g_cacheEngine->ForceInvalidate(requestResult.Value().key);
                result.succeeded = invalidateResult.IsOk();
                result.errorCode = static_cast<std::uint32_t>(invalidateResult.Err().code);
                result.message = invalidateResult.IsOk() ? "invalidated" : invalidateResult.Err().message;
            }
            auto frame = Ipc::MessageCodec::EncodeOperationResult(Ipc::MessageType::InvalidateKeyResponse, result);
            (void)transport.SendFrame(frame);
            break;
        }

        default:
            break; // unknown/unsupported request type: drop silently rather than crash.
    }
}

// Background thread body: repeatedly accepts one client connection and
// serves exactly one request from it.
//
// AUDITED BUG (fixed): this loop used to rely on a synchronous, blocking
// AcceptOnce() with no way to interrupt it, forcing the shutdown path to
// DETACH this thread instead of joining it — meaning a real process
// could report itself fully stopped (SERVICE_STOPPED) while this thread
// was still alive and could still be about to touch shared state.
// Win32NamedPipeServer::AcceptOnce() is now built on overlapped I/O and
// races the accept against IServer::RequestShutdown() internally (see
// Win32NamedPipeTransport.cpp), so it returns
// ErrorCode::ServiceStopping promptly once OnServiceStop() calls
// g_ipcServer->RequestShutdown() — no detach needed; see OnServiceStop().
void IpcServerThreadBody() {
    while (!g_ipcShouldStop.load()) {
        auto transportResult = g_ipcServer->AcceptOnce();
        if (g_ipcShouldStop.load()) return;
        if (!transportResult) {
            if (transportResult.Err().code == Common::ErrorCode::ServiceStopping) {
                // Expected, deterministic shutdown signal — not a real
                // error condition worth logging as a warning.
                return;
            }
            if (g_logger) {
                g_logger->Log(Logging::LogLevel::Warning, "Ipc",
                              "AcceptOnce failed: " + transportResult.Err().message);
            }
            continue;
        }
        HandleOneIpcRequest(*transportResult.Value());
        transportResult.Value()->Close();
    }
}

Common::Result<void> OnServiceStart() {
    Configuration::AppConfig config; // Stage 2 default config; see Stage 1 note on real deployment paths.

    auto sinkResult = Logging::CreateFileLogSink("QuantumCacheService.log");
    if (sinkResult) {
        g_logger = std::make_shared<Logging::Logger>();
        g_logger->AddSink(std::shared_ptr<Logging::ILogSink>(sinkResult.Value().release()));
        g_logger->Log(Logging::LogLevel::Info, "Service", "QuantumCacheService starting (Stage 2).");
    }

    auto configStoreResult = Configuration::CreateJsonConfigStore("QuantumCacheService.config.json");
    if (configStoreResult) {
        auto loadResult = configStoreResult.Value()->Load();
        if (loadResult) {
            config = loadResult.Value();
        } else if (g_logger) {
            g_logger->Log(Logging::LogLevel::Info, "Configuration",
                          "No valid config file found; using built-in defaults.");
        }
    }

    // ---- recovery initialization ----
    auto markerFile = Storage::OpenFile(config.stateDirectory + L"\\session.marker",
                                         Storage::OpenMode::OpenOrCreate);
    if (!markerFile) return Common::Result<void>::Failure(markerFile.Err());

    auto journalFile = Storage::OpenFile(config.stateDirectory + L"\\writeahead.journal",
                                          Storage::OpenMode::OpenOrCreate);
    if (!journalFile) return Common::Result<void>::Failure(journalFile.Err());

    auto marker = PowerResilience::CreateSessionMarker(std::move(markerFile.Value()));
    if (!marker) return Common::Result<void>::Failure(marker.Err());

    auto journalResult = PowerResilience::CreateWriteAheadJournal(std::move(journalFile.Value()));
    if (!journalResult) return Common::Result<void>::Failure(journalResult.Err());
    std::shared_ptr<PowerResilience::IWriteAheadJournal> journal = std::move(journalResult.Value());

    auto recoveryManagerResult = PowerResilience::CreateRecoveryManager(std::move(marker.Value()), journal);
    if (!recoveryManagerResult) return Common::Result<void>::Failure(recoveryManagerResult.Err());
    g_recoveryManager = std::move(recoveryManagerResult.Value());

    // Cache engine is constructed now (NOT-READY) so its ReplayFromJournal()
    // can be invoked from inside the replay callback below, but it is not
    // marked ready until InitializeAndRecover() itself has returned
    // success — see ICacheEngine.h contract.
    auto backingStoreResult = Storage::OpenFileBackingStore(config.backingStoreDataFile);
    if (!backingStoreResult) return Common::Result<void>::Failure(backingStoreResult.Err());
    std::shared_ptr<Storage::IBackingStore> backingStore = std::move(backingStoreResult.Value());

    auto engineOptions = BuildEngineOptions(config);
    auto engineResult = CoreEngine::CreateCacheEngine(engineOptions, backingStore, journal, g_logger);
    if (!engineResult) return Common::Result<void>::Failure(engineResult.Err());
    g_cacheEngine = std::move(engineResult.Value());

    // ---- journal validation/replay (only invoked if actually needed) ----
    auto initResult = g_recoveryManager->InitializeAndRecover([&]() -> Common::Result<void> {
        if (g_logger) {
            g_logger->Log(Logging::LogLevel::Warning, "PowerResilience",
                          "Unclean shutdown detected at startup (possible power loss). "
                          "Replaying write-ahead journal into the cache engine before "
                          "allowing any cache access.");
        }
        return g_cacheEngine->ReplayFromJournal();
    });

    if (!initResult) {
        if (g_logger) {
            g_logger->Log(Logging::LogLevel::Critical, "PowerResilience",
                          "Recovery failed at startup; refusing to report SERVICE_RUNNING. "
                          "Cache engine will remain permanently NOT-READY for this process "
                          "lifetime: " + initResult.Err().message);
        }
        return initResult; // fail safely: never mark the engine ready after a failed recovery.
    }

    // ---- recovery completion -> cache engine initialization ----
    auto markReadyResult = g_cacheEngine->MarkRecoveryComplete();
    if (!markReadyResult) return markReadyResult;

    if (g_logger) {
        g_logger->Log(Logging::LogLevel::Info, "PowerResilience",
                      std::string("Recovery state: ") + PowerResilience::ToString(g_recoveryManager->CurrentState()));
        g_logger->Log(Logging::LogLevel::Info, "CoreEngine", "Cache engine initialized and ready.");
    }

    // ---- only then: normal cache operations (IPC server begins accepting) ----
    auto ipcServerResult = Ipc::CreateNamedPipeServer(config.ipcPipeName);
    if (!ipcServerResult) {
        // Not being able to serve IPC status/management requests is
        // unfortunate but must not be conflated with cache-engine safety:
        // the engine itself is ready and correct. Log and continue rather
        // than failing the whole service over an optional management
        // surface.
        if (g_logger) {
            g_logger->Log(Logging::LogLevel::Error, "Ipc",
                          "Failed to create IPC server; continuing without remote status/management: " +
                              ipcServerResult.Err().message);
        }
    } else {
        g_ipcServer = std::move(ipcServerResult.Value());
        g_ipcShouldStop.store(false);
        g_ipcThread = std::thread(IpcServerThreadBody);
    }

    return Common::Result<void>::Success();
}

Common::Result<void> OnServiceStop() {
    if (g_logger) {
        g_logger->Log(Logging::LogLevel::Info, "Service",
                      "QuantumCacheService stopping (orderly): no longer accepting unsafe new operations.");
    }

    // AUDITED BUG (fixed): previously this set the stop flag and then
    // DETACHED the IPC thread rather than joining it, because the old
    // synchronous ConnectNamedPipe() call had no way to be interrupted.
    // Now that Win32NamedPipeServer supports overlapped I/O with an
    // explicit stop event (RequestShutdown()), we can deterministically
    // unblock any pending AcceptOnce() call and then JOIN the thread,
    // guaranteeing it has fully exited (and touches no shared state any
    // more) before OnServiceStop() returns and the service reports
    // itself stopped.
    g_ipcShouldStop.store(true);
    if (g_ipcServer) {
        g_ipcServer->RequestShutdown();
    }
    if (g_ipcThread.joinable()) {
        g_ipcThread.join();
    }

    // ---- flush according to configured safe policy, persist required state ----
    if (g_cacheEngine) {
        auto shutdownResult = g_cacheEngine->Shutdown();
        if (!shutdownResult && g_logger) {
            g_logger->Log(Logging::LogLevel::Warning, "CoreEngine",
                          "Cache engine shutdown reported an issue (data remains safely journaled): " +
                              shutdownResult.Err().message);
        }
    }

    // ---- mark clean shutdown ----
    if (g_recoveryManager) {
        auto result = g_recoveryManager->MarkCleanShutdown();
        if (!result && g_logger) {
            g_logger->Log(Logging::LogLevel::Error, "PowerResilience",
                          "Failed to mark clean shutdown; next startup may report an "
                          "unclean shutdown even though this stop was orderly.");
        }
        return result;
    }
    return Common::Result<void>::Success();
}

Common::Result<void> OnServicePreShutdown() {
    if (g_logger) {
        g_logger->Log(Logging::LogLevel::Info, "Service",
                      "PRESHUTDOWN received: proactively flushing dirty cache entries "
                      "while the larger PRESHUTDOWN time budget is available.");
    }
    if (g_cacheEngine) {
        auto flushResult = g_cacheEngine->FlushAll();
        if (!flushResult && g_logger) {
            g_logger->Log(Logging::LogLevel::Warning, "CoreEngine",
                          "PRESHUTDOWN flush did not fully succeed (data remains safely "
                          "journaled and will be recovered on next startup): " +
                              flushResult.Err().message);
        }
    }
    return Common::Result<void>::Success();
}

} // namespace

int wmain() {
    auto hostResult = QuantumCache::Service::CreateServiceHost(kServiceName);
    if (!hostResult) {
        return 1;
    }

    QuantumCache::Service::ServiceCallbacks callbacks;
    callbacks.onStart = &OnServiceStart;
    callbacks.onStop = &OnServiceStop;
    callbacks.onPreShutdown = &OnServicePreShutdown;

    auto runResult = hostResult.Value()->Run(callbacks);
    return runResult.IsOk() ? 0 : 1;
}
