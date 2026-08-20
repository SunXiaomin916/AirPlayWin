#pragma once

#include <Windows.h>
#include <powrprof.h>

#include <atomic>
#include <cstdint>
#include <optional>

namespace airplaywin::windows::system {

enum class PowerLifecycleEvent : std::uint8_t {
    Suspend,
    Resume,
};

struct PowerEventDiagnostics final {
    bool running{false};
    std::uint64_t suspend_events{0U};
    std::uint64_t resume_events{0U};
    std::uint32_t last_error{0U};
};

class WindowsPowerEventMonitor final {
public:
    WindowsPowerEventMonitor() = default;
    ~WindowsPowerEventMonitor();

    WindowsPowerEventMonitor(const WindowsPowerEventMonitor&) = delete;
    WindowsPowerEventMonitor& operator=(const WindowsPowerEventMonitor&) = delete;

    [[nodiscard]] bool Start() noexcept;
    void Stop() noexcept;
    [[nodiscard]] std::optional<PowerLifecycleEvent> Poll() noexcept;
    [[nodiscard]] PowerEventDiagnostics Diagnostics() const noexcept;

    // Kept public so deterministic tests can drive the exact Windows broadcast values.
    void RecordPowerBroadcast(std::uint32_t type) noexcept;

private:
    static ULONG CALLBACK NotificationCallback(void* context,
                                               ULONG type,
                                               void* setting) noexcept;

    static constexpr std::uint32_t kSuspendPending = 1U;
    static constexpr std::uint32_t kResumePending = 2U;

    HPOWERNOTIFY registration_{nullptr};
    std::atomic<std::uint32_t> pending_events_{0U};
    std::atomic<std::uint64_t> suspend_events_{0U};
    std::atomic<std::uint64_t> resume_events_{0U};
    std::atomic<std::uint32_t> last_error_{0U};
    std::atomic<bool> running_{false};
};

}  // namespace airplaywin::windows::system
