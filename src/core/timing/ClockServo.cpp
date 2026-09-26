#include "core/timing/ClockServo.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace airplaywin::timing {
namespace {

[[nodiscard]] bool IsFiniteUnitInterval(const double value) noexcept {
    return std::isfinite(value) && value > 0.0 && value <= 1.0;
}

[[nodiscard]] std::int64_t SaturatingRoundToInt64(const long double value) noexcept {
    if (value >= static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (value <= static_cast<long double>(std::numeric_limits<std::int64_t>::min())) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(std::llround(value));
}

}  // namespace

bool ClockServoConfig::IsValid() const noexcept {
    return local_clock_frequency > 0 && local_clock_frequency <= 1'000'000'000LL &&
           minimum_lock_samples >= 3U && minimum_lock_samples <= 1'024U &&
           relock_samples >= 1U && relock_samples <= minimum_lock_samples &&
           maximum_rtt_microseconds >= 1U &&
           outlier_threshold_microseconds >= 100U &&
           hard_resync_threshold_microseconds > outlier_threshold_microseconds &&
           holdover_after_microseconds >= 10'000U &&
           unlock_after_microseconds > holdover_after_microseconds &&
           std::isfinite(maximum_absolute_drift_ppm) &&
           maximum_absolute_drift_ppm >= 10.0 && maximum_absolute_drift_ppm <= 10'000.0 &&
           IsFiniteUnitInterval(drift_filter_alpha) &&
           IsFiniteUnitInterval(offset_filter_alpha) &&
           std::isfinite(maximum_rate_slew_ppm_per_second) &&
           maximum_rate_slew_ppm_per_second > 0.0 &&
           maximum_rate_slew_ppm_per_second <= 10'000.0;
}

ClockServo::ClockServo(const ClockServoConfig config) : config_(config) {
    if (!config_.IsValid()) {
        throw std::invalid_argument("invalid clock servo configuration");
    }
    diagnostics_.state = ClockServoState::Unlocked;
}

bool ClockServo::AddSample(const ClockSyncSample& sample) noexcept {
    std::scoped_lock lock{mutex_};
    if (sample.local_receive_qpc <= 0 ||
        sample.round_trip_nanoseconds / 1'000U > config_.maximum_rtt_microseconds) {
        ++diagnostics_.rejected_samples;
        diagnostics_.last_error = 1U;
        return false;
    }

    const auto half_rtt_ticks = SaturatingRoundToInt64(
        static_cast<long double>(sample.round_trip_nanoseconds) *
        static_cast<long double>(config_.local_clock_frequency) / 2'000'000'000.0L);
    if (sample.local_receive_qpc < std::numeric_limits<std::int64_t>::min() + half_rtt_ticks) {
        ++diagnostics_.rejected_samples;
        diagnostics_.last_error = 2U;
        return false;
    }
    const auto corrected_local_qpc = sample.local_receive_qpc - half_rtt_ticks;
    diagnostics_.rtt_microseconds = sample.round_trip_nanoseconds / 1'000U;

    const bool master_changed = diagnostics_.source_clock_identity != 0U &&
                                sample.source_clock_identity != 0U &&
                                diagnostics_.source_clock_identity !=
                                    sample.source_clock_identity;
    if (master_changed) {
        ++diagnostics_.master_change_events;
        ++diagnostics_.generation;
        have_model_ = false;
        diagnostics_.state = ClockServoState::Acquiring;
        diagnostics_.locked = false;
    }
    if (sample.source_clock_identity != 0U) {
        diagnostics_.source_clock_identity = sample.source_clock_identity;
    }

    UpdateStateLocked(sample.local_receive_qpc);
    if (have_model_ && diagnostics_.state == ClockServoState::Unlocked) {
        ResetModelLocked(sample, corrected_local_qpc);
        ++diagnostics_.accepted_samples;
        acquisition_samples_ = 1U;
        return true;
    }
    if (!have_model_) {
        ResetModelLocked(sample, corrected_local_qpc);
        ++diagnostics_.accepted_samples;
        acquisition_samples_ = 1U;
        return true;
    }

    if (sample.remote_time_nanoseconds <= last_remote_nanoseconds_ ||
        corrected_local_qpc <= last_local_qpc_) {
        ++diagnostics_.rejected_samples;
        diagnostics_.last_error = 3U;
        return false;
    }

    const auto predicted = MapLocked(sample.remote_time_nanoseconds);
    if (!predicted.has_value()) {
        ++diagnostics_.rejected_samples;
        diagnostics_.last_error = 4U;
        return false;
    }
    const auto residual_ticks = corrected_local_qpc - *predicted;
    const auto residual_microseconds = TicksToMicroseconds(
        residual_ticks >= 0 ? residual_ticks : -residual_ticks);
    diagnostics_.uncertainty_microseconds = diagnostics_.uncertainty_microseconds == 0U
                                                ? residual_microseconds
                                                : (diagnostics_.uncertainty_microseconds * 7U +
                                                   residual_microseconds + 4U) /
                                                      8U;

    if (residual_microseconds >= config_.hard_resync_threshold_microseconds &&
        diagnostics_.locked) {
        ++diagnostics_.hard_resync_events;
        ++diagnostics_.generation;
        diagnostics_.hard_resync_pending = true;
        ResetModelLocked(sample, corrected_local_qpc);
        ++diagnostics_.accepted_samples;
        acquisition_samples_ = 1U;
        return true;
    }
    if (residual_microseconds > config_.outlier_threshold_microseconds) {
        ++diagnostics_.rejected_samples;
        ++diagnostics_.outlier_samples;
        diagnostics_.last_error = 5U;
        return false;
    }

    // Estimate oscillator slope over the complete acquisition window. Pairwise slopes amplify
    // receive scheduling jitter, especially at the 125 ms Sync cadence used by some senders.
    const auto remote_delta = sample.remote_time_nanoseconds -
                              rate_anchor_remote_nanoseconds_;
    const auto local_delta = corrected_local_qpc - rate_anchor_local_qpc_;
    const auto observed_ratio =
        static_cast<long double>(local_delta) * 1'000'000'000.0L /
        (static_cast<long double>(remote_delta) * config_.local_clock_frequency);
    const auto observed_drift_ppm =
        static_cast<double>((observed_ratio - 1.0L) * 1'000'000.0L);
    const auto instantaneous_limit =
        std::max(10'000.0, config_.maximum_absolute_drift_ppm * 20.0);
    if (!std::isfinite(observed_drift_ppm) ||
        std::abs(observed_drift_ppm) > instantaneous_limit) {
        ++diagnostics_.rejected_samples;
        ++diagnostics_.outlier_samples;
        diagnostics_.last_error = 6U;
        return false;
    }

    const auto bounded_drift = std::clamp(observed_drift_ppm,
                                          -config_.maximum_absolute_drift_ppm,
                                          config_.maximum_absolute_drift_ppm);
    diagnostics_.drift_ppm += config_.drift_filter_alpha *
                              (bounded_drift - diagnostics_.drift_ppm);
    const auto offset_adjustment = SaturatingRoundToInt64(
        static_cast<long double>(residual_ticks) * config_.offset_filter_alpha);
    diagnostics_.anchor_local_qpc =
        SaturatingRoundToInt64(static_cast<long double>(diagnostics_.anchor_local_qpc) +
                               offset_adjustment);
    diagnostics_.offset_microseconds = residual_ticks >= 0
                                            ? static_cast<std::int64_t>(residual_microseconds)
                                            : -static_cast<std::int64_t>(residual_microseconds);
    last_remote_nanoseconds_ = sample.remote_time_nanoseconds;
    last_local_qpc_ = corrected_local_qpc;
    UpdateRateCorrectionLocked(sample.local_receive_qpc);
    ++diagnostics_.accepted_samples;
    diagnostics_.last_error = 0U;

    if (diagnostics_.state == ClockServoState::Holdover) {
        diagnostics_.state = ClockServoState::Relocking;
        diagnostics_.locked = true;
        relock_good_samples_ = 1U;
        ++diagnostics_.relock_events;
    } else if (diagnostics_.state == ClockServoState::Relocking) {
        if (++relock_good_samples_ >= config_.relock_samples) {
            diagnostics_.state = ClockServoState::Locked;
        }
    } else if (diagnostics_.state == ClockServoState::Acquiring) {
        if (++acquisition_samples_ >= config_.minimum_lock_samples) {
            diagnostics_.state = ClockServoState::Locked;
            diagnostics_.locked = true;
            ++diagnostics_.lock_events;
        }
    }
    return true;
}

void ClockServo::Update(const std::int64_t now_qpc) noexcept {
    std::scoped_lock lock{mutex_};
    UpdateStateLocked(now_qpc);
    UpdateRateCorrectionLocked(now_qpc);
}

void ClockServo::Reset() noexcept {
    std::scoped_lock lock{mutex_};
    const auto generation = diagnostics_.generation + 1U;
    const auto source = diagnostics_.source_clock_identity;
    diagnostics_ = {};
    diagnostics_.state = ClockServoState::Unlocked;
    diagnostics_.generation = generation;
    diagnostics_.source_clock_identity = source;
    have_model_ = false;
    last_remote_nanoseconds_ = 0U;
    last_local_qpc_ = 0;
    rate_anchor_remote_nanoseconds_ = 0U;
    rate_anchor_local_qpc_ = 0;
    last_rate_update_qpc_ = 0;
    acquisition_samples_ = 0U;
    relock_good_samples_ = 0U;
}

std::optional<std::int64_t> ClockServo::RemoteToLocalQpc(
    const std::uint64_t remote_time_nanoseconds) const noexcept {
    std::scoped_lock lock{mutex_};
    if (!diagnostics_.locked || diagnostics_.state == ClockServoState::Unlocked) {
        return std::nullopt;
    }
    return MapLocked(remote_time_nanoseconds);
}

double ClockServo::RateCorrection() const noexcept {
    std::scoped_lock lock{mutex_};
    return diagnostics_.locked ? diagnostics_.rate_correction : 1.0;
}

bool ClockServo::ConsumeHardResyncRequest() noexcept {
    std::scoped_lock lock{mutex_};
    return std::exchange(diagnostics_.hard_resync_pending, false);
}

ClockServoDiagnostics ClockServo::Diagnostics() const noexcept {
    std::scoped_lock lock{mutex_};
    return diagnostics_;
}

void ClockServo::ResetModelLocked(const ClockSyncSample& sample,
                                  const std::int64_t corrected_local_qpc) noexcept {
    have_model_ = true;
    diagnostics_.state = ClockServoState::Acquiring;
    diagnostics_.locked = false;
    diagnostics_.anchor_remote_nanoseconds = sample.remote_time_nanoseconds;
    diagnostics_.anchor_local_qpc = corrected_local_qpc;
    diagnostics_.offset_microseconds = 0;
    diagnostics_.drift_ppm = 0.0;
    diagnostics_.rate_correction = 1.0;
    diagnostics_.last_sync_age_microseconds = 0U;
    last_remote_nanoseconds_ = sample.remote_time_nanoseconds;
    last_local_qpc_ = corrected_local_qpc;
    rate_anchor_remote_nanoseconds_ = sample.remote_time_nanoseconds;
    rate_anchor_local_qpc_ = corrected_local_qpc;
    last_rate_update_qpc_ = sample.local_receive_qpc;
    acquisition_samples_ = 0U;
    relock_good_samples_ = 0U;
}

void ClockServo::UpdateStateLocked(const std::int64_t now_qpc) noexcept {
    if (!have_model_ || now_qpc <= last_local_qpc_) {
        return;
    }
    const auto age = TicksToMicroseconds(now_qpc - last_local_qpc_);
    diagnostics_.last_sync_age_microseconds = age;
    if (age >= config_.unlock_after_microseconds) {
        diagnostics_.state = ClockServoState::Unlocked;
        diagnostics_.locked = false;
        diagnostics_.rate_correction = 1.0;
        return;
    }
    if (age >= config_.holdover_after_microseconds &&
        (diagnostics_.state == ClockServoState::Locked ||
         diagnostics_.state == ClockServoState::Relocking)) {
        diagnostics_.state = ClockServoState::Holdover;
        diagnostics_.locked = true;
        relock_good_samples_ = 0U;
        ++diagnostics_.holdover_events;
    }
}

void ClockServo::UpdateRateCorrectionLocked(const std::int64_t now_qpc) noexcept {
    if (!have_model_ || now_qpc <= last_rate_update_qpc_) {
        return;
    }
    const auto elapsed_seconds = static_cast<long double>(now_qpc - last_rate_update_qpc_) /
                                 config_.local_clock_frequency;
    const auto target = 1.0 / (1.0 + diagnostics_.drift_ppm / 1'000'000.0);
    const auto maximum_step = static_cast<double>(
        elapsed_seconds * config_.maximum_rate_slew_ppm_per_second / 1'000'000.0L);
    diagnostics_.rate_correction += std::clamp(
        target - diagnostics_.rate_correction, -maximum_step, maximum_step);
    last_rate_update_qpc_ = now_qpc;
}

std::optional<std::int64_t> ClockServo::MapLocked(
    const std::uint64_t remote_time_nanoseconds) const noexcept {
    if (!have_model_) {
        return std::nullopt;
    }
    const auto remote_delta = static_cast<long double>(remote_time_nanoseconds) -
                              diagnostics_.anchor_remote_nanoseconds;
    const auto rate = 1.0L + static_cast<long double>(diagnostics_.drift_ppm) /
                                  1'000'000.0L;
    const auto local_delta = remote_delta * config_.local_clock_frequency * rate /
                             1'000'000'000.0L;
    const auto mapped = static_cast<long double>(diagnostics_.anchor_local_qpc) + local_delta;
    if (mapped > static_cast<long double>(std::numeric_limits<std::int64_t>::max()) ||
        mapped < static_cast<long double>(std::numeric_limits<std::int64_t>::min())) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(std::llround(mapped));
}

std::uint64_t ClockServo::TicksToMicroseconds(const std::int64_t ticks) const noexcept {
    if (ticks <= 0) {
        return 0U;
    }
    const auto value = static_cast<std::uint64_t>(ticks);
    const auto frequency = static_cast<std::uint64_t>(config_.local_clock_frequency);
    return value / frequency * 1'000'000U +
           value % frequency * 1'000'000U / frequency;
}

}  // namespace airplaywin::timing
