#include "platform/windows/system/WindowsPowerEventMonitor.h"

namespace airplaywin::windows::system {

WindowsPowerEventMonitor::~WindowsPowerEventMonitor() {
    Stop();
}

bool WindowsPowerEventMonitor::Start() noexcept {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS parameters{
        .Callback = &WindowsPowerEventMonitor::NotificationCallback,
        .Context = this,
    };
    HPOWERNOTIFY registration = nullptr;
    const auto status = PowerRegisterSuspendResumeNotification(
        DEVICE_NOTIFY_CALLBACK, &parameters, &registration);
    if (status != ERROR_SUCCESS) {
        last_error_.store(status, std::memory_order_release);
        return false;
    }
    registration_ = registration;
    pending_events_.store(0U, std::memory_order_release);
    last_error_.store(ERROR_SUCCESS, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    return true;
}

void WindowsPowerEventMonitor::Stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (registration_ != nullptr) {
        const auto status = PowerUnregisterSuspendResumeNotification(registration_);
        if (status != ERROR_SUCCESS) {
            last_error_.store(status, std::memory_order_release);
        }
        registration_ = nullptr;
    }
    pending_events_.store(0U, std::memory_order_release);
}

std::optional<PowerLifecycleEvent> WindowsPowerEventMonitor::Poll() noexcept {
    auto pending = pending_events_.load(std::memory_order_acquire);
    while (pending != 0U) {
        const auto selected = (pending & kSuspendPending) != 0U ? kSuspendPending
                                                                : kResumePending;
        const auto remaining = pending & ~selected;
        if (pending_events_.compare_exchange_weak(pending, remaining,
                                                  std::memory_order_acq_rel)) {
            return selected == kSuspendPending ? PowerLifecycleEvent::Suspend
                                               : PowerLifecycleEvent::Resume;
        }
    }
    return std::nullopt;
}

PowerEventDiagnostics WindowsPowerEventMonitor::Diagnostics() const noexcept {
    return {
        .running = running_.load(std::memory_order_acquire),
        .suspend_events = suspend_events_.load(std::memory_order_relaxed),
        .resume_events = resume_events_.load(std::memory_order_relaxed),
        .last_error = last_error_.load(std::memory_order_relaxed),
    };
}

void WindowsPowerEventMonitor::RecordPowerBroadcast(const std::uint32_t type) noexcept {
    if (type == PBT_APMSUSPEND) {
        suspend_events_.fetch_add(1U, std::memory_order_relaxed);
        pending_events_.fetch_or(kSuspendPending, std::memory_order_release);
    } else if (type == PBT_APMRESUMEAUTOMATIC || type == PBT_APMRESUMESUSPEND ||
               type == PBT_APMRESUMECRITICAL) {
        resume_events_.fetch_add(1U, std::memory_order_relaxed);
        pending_events_.fetch_or(kResumePending, std::memory_order_release);
    }
}

ULONG CALLBACK WindowsPowerEventMonitor::NotificationCallback(void* const context,
                                                               const ULONG type,
                                                               void* const setting) noexcept {
    static_cast<void>(setting);
    if (context != nullptr) {
        static_cast<WindowsPowerEventMonitor*>(context)->RecordPowerBroadcast(type);
    }
    return ERROR_SUCCESS;
}

}  // namespace airplaywin::windows::system
