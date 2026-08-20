#pragma once

#include <cstdint>

namespace airplaywin::lifecycle {

enum class ReceiverRuntimeState : std::uint8_t {
    Running,
    Suspended,
    RecoveryPending,
    Stopped,
};

struct RecoveryDiagnostics final {
    ReceiverRuntimeState state{ReceiverRuntimeState::Running};
    std::uint64_t recovery_generation{0U};
    std::uint64_t suspend_events{0U};
    std::uint64_t resume_events{0U};
    std::uint64_t recovery_attempts{0U};
    std::uint64_t recovery_successes{0U};
    std::uint64_t recovery_failures{0U};
    std::uint32_t last_error{0U};
};

// Platform-neutral state machine. Platform callbacks only enqueue events; the
// application thread performs service stop/start and records the result here.
class RecoveryCoordinator final {
public:
    void OnSuspend() noexcept;
    void OnResume() noexcept;
    void RequestRecovery(std::uint32_t reason_error = 0U) noexcept;
    void RecordRecoveryResult(bool succeeded, std::uint32_t error = 0U) noexcept;
    void Stop() noexcept;

    [[nodiscard]] bool RecoveryRequired() const noexcept;
    [[nodiscard]] bool IsSuspended() const noexcept;
    [[nodiscard]] RecoveryDiagnostics Diagnostics() const noexcept;

private:
    RecoveryDiagnostics diagnostics_{};
};

}  // namespace airplaywin::lifecycle
