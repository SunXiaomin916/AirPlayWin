#include "core/timing/BufferedRtpTimingEngine.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace airplaywin::timing {

BufferedRtpTimingEngine::BufferedRtpTimingEngine(
    const BufferedRtpTimingConfig config,
    std::unique_ptr<IMonotonicClock> clock)
    : config_(config), clock_(std::move(clock)) {
    if (!config_.IsValid() || !clock_) {
        throw std::invalid_argument("invalid buffered RTP timing configuration");
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

void BufferedRtpTimingEngine::Reset(
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

std::optional<std::int64_t> BufferedRtpTimingEngine::RemoteToLocalQpc(
    const std::uint64_t remote_time) noexcept {
    std::scoped_lock lock{mutex_};
    const auto remote_timestamp = static_cast<std::uint32_t>(remote_time);
    const auto now = clock_->Now();
    if (!diagnostics_.locked) {
        LockTimeline(remote_timestamp, now);
    }
    const auto target = MapLocked(remote_timestamp);
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

TimingDiagnostics BufferedRtpTimingEngine::Diagnostics() const noexcept {
    std::scoped_lock lock{mutex_};
    return diagnostics_;
}

void BufferedRtpTimingEngine::LockTimeline(const std::uint32_t remote_timestamp,
                                           const std::int64_t now_qpc) noexcept {
    const auto buffer_ticks =
        clock_frequency_ * static_cast<std::int64_t>(config_.target_buffer_milliseconds) /
        1'000LL;
    if (now_qpc > std::numeric_limits<std::int64_t>::max() - buffer_ticks) {
        diagnostics_.last_error = 1U;
        return;
    }
    diagnostics_.locked = true;
    diagnostics_.anchor_remote_time = remote_timestamp;
    diagnostics_.anchor_target_qpc = now_qpc + buffer_ticks;
}

std::optional<std::int64_t> BufferedRtpTimingEngine::MapLocked(
    const std::uint32_t remote_timestamp) noexcept {
    if (!diagnostics_.locked) {
        return std::nullopt;
    }
    const auto delta = static_cast<std::int64_t>(static_cast<std::int32_t>(
        remote_timestamp - static_cast<std::uint32_t>(diagnostics_.anchor_remote_time)));
    const auto scaled = delta * clock_frequency_ /
                        static_cast<std::int64_t>(config_.remote_clock_rate);
    if ((scaled > 0 && diagnostics_.anchor_target_qpc >
                           std::numeric_limits<std::int64_t>::max() - scaled) ||
        (scaled < 0 && diagnostics_.anchor_target_qpc <
                           std::numeric_limits<std::int64_t>::min() - scaled)) {
        diagnostics_.last_error = 2U;
        return std::nullopt;
    }
    return diagnostics_.anchor_target_qpc + scaled;
}

std::uint64_t BufferedRtpTimingEngine::TicksToMicroseconds(
    const std::int64_t ticks) const noexcept {
    if (ticks <= 0 || clock_frequency_ <= 0) {
        return 0U;
    }
    const auto value = static_cast<std::uint64_t>(ticks);
    const auto frequency = static_cast<std::uint64_t>(clock_frequency_);
    const auto whole_seconds = value / frequency;
    if (whole_seconds > std::numeric_limits<std::uint64_t>::max() / 1'000'000U) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return whole_seconds * 1'000'000U +
           (value % frequency) * 1'000'000U / frequency;
}

}  // namespace airplaywin::timing
