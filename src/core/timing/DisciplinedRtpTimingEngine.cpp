#include "core/timing/DisciplinedRtpTimingEngine.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace airplaywin::timing {

DisciplinedRtpTimingEngine::DisciplinedRtpTimingEngine(
    const DisciplinedRtpTimingConfig config,
    std::shared_ptr<PtpClockDomain> clock_domain,
    std::unique_ptr<IMonotonicClock> clock)
    : config_(config), clock_domain_(std::move(clock_domain)), clock_(std::move(clock)) {
    if (!config_.IsValid() || !clock_domain_ || !clock_) {
        throw std::invalid_argument("invalid disciplined RTP timing configuration");
    }
    clock_frequency_ = clock_->Frequency();
    if (clock_frequency_ <= 0 || clock_frequency_ > 1'000'000'000LL) {
        throw std::invalid_argument("invalid monotonic clock frequency");
    }
    diagnostics_.enabled = true;
    diagnostics_.remote_clock_rate = config_.remote_clock_rate;
    diagnostics_.target_buffer_microseconds =
        static_cast<std::uint64_t>(config_.target_buffer_milliseconds) * 1'000U;
}

void DisciplinedRtpTimingEngine::Reset(
    const std::optional<std::uint64_t> remote_time) noexcept {
    std::scoped_lock lock{mutex_};
    ++diagnostics_.generation;
    diagnostics_.locked = false;
    diagnostics_.anchor_remote_time = 0U;
    diagnostics_.anchor_target_qpc = 0;
    diagnostics_.last_remote_time = 0U;
    diagnostics_.last_target_qpc = 0;
    diagnostics_.last_error = 0U;
    if (remote_time.has_value()) {
        LockTimeline(static_cast<std::uint32_t>(*remote_time), clock_->Now());
    }
}

std::optional<std::int64_t> DisciplinedRtpTimingEngine::RemoteToLocalQpc(
    const std::uint64_t remote_time) noexcept {
    std::scoped_lock lock{mutex_};
    const auto remote_timestamp = static_cast<std::uint32_t>(remote_time);
    const auto now = clock_->Now();
    clock_domain_->Update(now);
    diagnostics_.servo = clock_domain_->Diagnostics().servo;
    diagnostics_.locked = diagnostics_.servo.locked;
    if (diagnostics_.anchor_target_qpc == 0) {
        LockTimeline(remote_timestamp, now);
    }
    const auto target = MapLocked(remote_timestamp, diagnostics_.servo.rate_correction);
    if (!target.has_value()) {
        return std::nullopt;
    }
    ++diagnostics_.mapped_packets;
    diagnostics_.last_remote_time = remote_timestamp;
    diagnostics_.last_target_qpc = *target;
    if (now > *target) {
        ++diagnostics_.late_mappings;
        diagnostics_.maximum_lateness_microseconds = std::max(
            diagnostics_.maximum_lateness_microseconds,
            TicksToMicroseconds(now - *target));
    }
    return target;
}

TimingDiagnostics DisciplinedRtpTimingEngine::Diagnostics() const noexcept {
    std::scoped_lock lock{mutex_};
    auto result = diagnostics_;
    result.servo = clock_domain_->Diagnostics().servo;
    result.locked = result.servo.locked;
    return result;
}

double DisciplinedRtpTimingEngine::RateCorrection() const noexcept {
    return clock_domain_->RateCorrection();
}

bool DisciplinedRtpTimingEngine::ConsumeHardResyncRequest() noexcept {
    return clock_domain_->ConsumeHardResyncRequest();
}

void DisciplinedRtpTimingEngine::LockTimeline(const std::uint32_t remote_timestamp,
                                              const std::int64_t now_qpc) noexcept {
    const auto buffer_ticks =
        clock_frequency_ * static_cast<std::int64_t>(config_.target_buffer_milliseconds) /
        1'000LL;
    if (now_qpc <= 0 || now_qpc > std::numeric_limits<std::int64_t>::max() - buffer_ticks) {
        diagnostics_.last_error = 1U;
        return;
    }
    diagnostics_.anchor_remote_time = remote_timestamp;
    diagnostics_.anchor_target_qpc = now_qpc + buffer_ticks;
}

std::optional<std::int64_t> DisciplinedRtpTimingEngine::MapLocked(
    const std::uint32_t remote_timestamp,
    const double rate_correction) noexcept {
    if (diagnostics_.anchor_target_qpc == 0 || !std::isfinite(rate_correction) ||
        rate_correction < 0.98 || rate_correction > 1.02) {
        diagnostics_.last_error = 2U;
        return std::nullopt;
    }
    const auto delta = static_cast<std::int64_t>(static_cast<std::int32_t>(
        remote_timestamp - static_cast<std::uint32_t>(diagnostics_.anchor_remote_time)));
    const auto scaled = static_cast<long double>(delta) * clock_frequency_ /
                        (static_cast<long double>(config_.remote_clock_rate) *
                         rate_correction);
    const auto target = static_cast<long double>(diagnostics_.anchor_target_qpc) + scaled;
    if (target > static_cast<long double>(std::numeric_limits<std::int64_t>::max()) ||
        target < static_cast<long double>(std::numeric_limits<std::int64_t>::min())) {
        diagnostics_.last_error = 3U;
        return std::nullopt;
    }
    return static_cast<std::int64_t>(std::llround(target));
}

std::uint64_t DisciplinedRtpTimingEngine::TicksToMicroseconds(
    const std::int64_t ticks) const noexcept {
    if (ticks <= 0) {
        return 0U;
    }
    const auto value = static_cast<std::uint64_t>(ticks);
    const auto frequency = static_cast<std::uint64_t>(clock_frequency_);
    return value / frequency * 1'000'000U +
           value % frequency * 1'000'000U / frequency;
}

}  // namespace airplaywin::timing
