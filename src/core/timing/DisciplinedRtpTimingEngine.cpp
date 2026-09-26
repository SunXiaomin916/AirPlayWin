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
    std::unique_ptr<IMonotonicClock> clock,
    std::shared_ptr<RtpPtpPhaseTimeline> phase_timeline)
    : config_(config),
      clock_domain_(std::move(clock_domain)),
      clock_(std::move(clock)),
      phase_timeline_(std::move(phase_timeline)) {
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
    diagnostics_.endpoint_latency_offset_microseconds =
        config_.endpoint_latency_offset_microseconds;
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
    diagnostics_.absolute_phase_active = false;
    diagnostics_.last_error = 0U;
    if (remote_time.has_value() && !config_.require_phase_anchor) {
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
    if (phase_timeline_) {
        const auto phase = phase_timeline_->Diagnostics();
        diagnostics_.phase_anchor_available = phase.anchored;
        diagnostics_.phase_session_epoch = phase.anchor.session_epoch;
        diagnostics_.phase_remote_ptp_nanoseconds =
            phase.anchor.remote_ptp_nanoseconds;
        if (phase.anchored) {
            const bool master_matches = phase.anchor.master_clock_identity == 0U ||
                                        diagnostics_.servo.source_clock_identity == 0U ||
                                        phase.anchor.master_clock_identity ==
                                            diagnostics_.servo.source_clock_identity;
            const auto remote_ptp = master_matches
                                        ? phase_timeline_->RtpToRemotePtpNanoseconds(
                                              remote_timestamp)
                                        : std::nullopt;
            const auto target = remote_ptp.has_value()
                                    ? clock_domain_->RemoteToLocalQpc(*remote_ptp)
                                    : std::nullopt;
            if (target.has_value()) {
                const auto offset_ticks = static_cast<long double>(
                                              config_.endpoint_latency_offset_microseconds) *
                                          clock_frequency_ / 1'000'000.0L;
                const auto adjusted = static_cast<long double>(*target) - offset_ticks;
                if (adjusted >= static_cast<long double>(
                                    std::numeric_limits<std::int64_t>::min()) &&
                    adjusted <= static_cast<long double>(
                                    std::numeric_limits<std::int64_t>::max())) {
                    const auto mapped = static_cast<std::int64_t>(std::llround(adjusted));
                    diagnostics_.absolute_phase_active = true;
                    ++diagnostics_.absolute_phase_mappings;
                    ++diagnostics_.mapped_packets;
                    diagnostics_.last_remote_time = remote_timestamp;
                    diagnostics_.last_target_qpc = mapped;
                    if (now > mapped) {
                        ++diagnostics_.late_mappings;
                        diagnostics_.maximum_lateness_microseconds = std::max(
                            diagnostics_.maximum_lateness_microseconds,
                            TicksToMicroseconds(now - mapped));
                    }
                    return mapped;
                }
            }
            ++diagnostics_.phase_mapping_failures;
            diagnostics_.absolute_phase_active = false;
            diagnostics_.last_error = 4U;
            if (config_.require_phase_anchor) {
                return std::nullopt;
            }
        } else if (config_.require_phase_anchor) {
            ++diagnostics_.phase_mapping_failures;
            diagnostics_.absolute_phase_active = false;
            diagnostics_.last_error = 4U;
            return std::nullopt;
        }
    }
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

bool DisciplinedRtpTimingEngine::RequiresMappedTarget() const noexcept {
    return config_.require_phase_anchor;
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
