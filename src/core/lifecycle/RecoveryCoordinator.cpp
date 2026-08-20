#include "core/lifecycle/RecoveryCoordinator.h"

namespace airplaywin::lifecycle {

void RecoveryCoordinator::OnSuspend() noexcept {
    if (diagnostics_.state == ReceiverRuntimeState::Stopped) {
        return;
    }
    ++diagnostics_.suspend_events;
    if (diagnostics_.state != ReceiverRuntimeState::Suspended) {
        ++diagnostics_.recovery_generation;
    }
    diagnostics_.state = ReceiverRuntimeState::Suspended;
    diagnostics_.last_error = 0U;
}

void RecoveryCoordinator::OnResume() noexcept {
    if (diagnostics_.state == ReceiverRuntimeState::Stopped) {
        return;
    }
    ++diagnostics_.resume_events;
    if (diagnostics_.state != ReceiverRuntimeState::RecoveryPending) {
        ++diagnostics_.recovery_generation;
    }
    diagnostics_.state = ReceiverRuntimeState::RecoveryPending;
    diagnostics_.last_error = 0U;
}

void RecoveryCoordinator::RequestRecovery(const std::uint32_t reason_error) noexcept {
    if (diagnostics_.state == ReceiverRuntimeState::Stopped ||
        diagnostics_.state == ReceiverRuntimeState::Suspended) {
        return;
    }
    if (diagnostics_.state != ReceiverRuntimeState::RecoveryPending) {
        ++diagnostics_.recovery_generation;
    }
    diagnostics_.state = ReceiverRuntimeState::RecoveryPending;
    diagnostics_.last_error = reason_error;
}

void RecoveryCoordinator::RecordRecoveryResult(const bool succeeded,
                                                const std::uint32_t error) noexcept {
    if (diagnostics_.state != ReceiverRuntimeState::RecoveryPending) {
        return;
    }
    ++diagnostics_.recovery_attempts;
    if (succeeded) {
        ++diagnostics_.recovery_successes;
        diagnostics_.state = ReceiverRuntimeState::Running;
        diagnostics_.last_error = 0U;
    } else {
        ++diagnostics_.recovery_failures;
        diagnostics_.last_error = error;
    }
}

void RecoveryCoordinator::Stop() noexcept {
    diagnostics_.state = ReceiverRuntimeState::Stopped;
}

bool RecoveryCoordinator::RecoveryRequired() const noexcept {
    return diagnostics_.state == ReceiverRuntimeState::RecoveryPending;
}

bool RecoveryCoordinator::IsSuspended() const noexcept {
    return diagnostics_.state == ReceiverRuntimeState::Suspended;
}

RecoveryDiagnostics RecoveryCoordinator::Diagnostics() const noexcept {
    return diagnostics_;
}

}  // namespace airplaywin::lifecycle
