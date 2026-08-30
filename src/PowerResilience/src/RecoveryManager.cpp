// Orchestrates ISessionMarker + IWriteAheadJournal into the RecoveryState
// lifecycle described in RecoveryState.h. This is real control-flow logic,
// unit tested (see tests/PowerResilienceTests.cpp) by simulating an
// unclean shutdown (constructing a marker left in the "open" state,
// exactly as a power cut would leave it) and confirming recovery is
// correctly triggered, and that a replay failure is reported as
// RecoveryFailed rather than silently swallowed.
#include "QuantumCache/PowerResilience/IRecoveryManager.h"
#include "QuantumCache/PowerResilience/ISessionMarker.h"
#include "QuantumCache/PowerResilience/IWriteAheadJournal.h"

namespace QuantumCache::PowerResilience {
namespace {

using Common::Error;
using Common::ErrorCode;
using Common::Result;

class RecoveryManager final : public IRecoveryManager {
public:
    RecoveryManager(std::unique_ptr<ISessionMarker> marker,
                     std::shared_ptr<IWriteAheadJournal> journal)
        : marker_(std::move(marker)), journal_(std::move(journal)) {}

    Result<void> InitializeAndRecover(const JournalReplayHandler& onReplayNeeded) override {
        auto lastState = marker_->ReadLastState();
        if (!lastState) {
            state_ = RecoveryState::RecoveryFailed;
            return Result<void>::Failure(lastState.Err());
        }

        if (lastState.Value().closedCleanly) {
            state_ = RecoveryState::CleanShutdown;
        } else {
            state_ = RecoveryState::UncleanShutdownDetected;
            state_ = RecoveryState::RecoveryInProgress;

            if (onReplayNeeded) {
                auto replayResult = onReplayNeeded();
                if (!replayResult) {
                    state_ = RecoveryState::RecoveryFailed;
                    return Result<void>::Failure(replayResult.Err());
                }
            }

            state_ = RecoveryState::RecoveryComplete;
        }

        auto startResult = marker_->OnSessionStart();
        if (!startResult) {
            state_ = RecoveryState::RecoveryFailed;
            return Result<void>::Failure(startResult.Err());
        }

        if (state_ == RecoveryState::CleanShutdown) {
            state_ = RecoveryState::RecoveryComplete;
        }

        return Result<void>::Success();
    }

    RecoveryState CurrentState() const noexcept override { return state_; }

    Result<void> MarkCleanShutdown() override {
        return marker_->OnCleanShutdown();
    }

private:
    std::unique_ptr<ISessionMarker> marker_;
    std::shared_ptr<IWriteAheadJournal> journal_;
    RecoveryState state_{RecoveryState::Unknown};
};

} // namespace

Common::Result<std::unique_ptr<IRecoveryManager>> CreateRecoveryManager(
    std::unique_ptr<ISessionMarker> marker,
    std::shared_ptr<IWriteAheadJournal> journal) {
    if (!marker || !journal) {
        return Common::Result<std::unique_ptr<IRecoveryManager>>::Failure(
            Error{ErrorCode::InvalidArgument, "marker and journal must not be null", 0});
    }
    return Common::Result<std::unique_ptr<IRecoveryManager>>::Success(
        std::make_unique<RecoveryManager>(std::move(marker), std::move(journal)));
}

} // namespace QuantumCache::PowerResilience
