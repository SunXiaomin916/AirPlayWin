#include "core/timing/RtpPtpPhaseTimeline.h"

#include <cmath>
#include <limits>

namespace airplaywin::timing {

bool RtpPtpPhaseTimeline::Publish(const RtpPtpPhaseAnchor& anchor) noexcept {
    if (!anchor.IsValid()) {
        return false;
    }
    std::scoped_lock lock{mutex_};
    if (diagnostics_.anchored &&
        anchor.session_epoch < diagnostics_.anchor.session_epoch) {
        return false;
    }
    diagnostics_.anchored = true;
    diagnostics_.anchor = anchor;
    ++diagnostics_.generation;
    ++diagnostics_.published_anchors;
    return true;
}

void RtpPtpPhaseTimeline::Clear() noexcept {
    std::scoped_lock lock{mutex_};
    diagnostics_.anchored = false;
    diagnostics_.anchor = {};
    ++diagnostics_.generation;
    ++diagnostics_.cleared_anchors;
}

std::optional<std::uint64_t> RtpPtpPhaseTimeline::RtpToRemotePtpNanoseconds(
    const std::uint32_t rtp_timestamp) const noexcept {
    std::scoped_lock lock{mutex_};
    if (!diagnostics_.anchored) {
        ++diagnostics_.rejected_mappings;
        return std::nullopt;
    }
    const auto delta_frames = static_cast<std::int64_t>(static_cast<std::int32_t>(
        rtp_timestamp - diagnostics_.anchor.rtp_timestamp));
    constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000LL;
    const auto scaled = delta_frames * kNanosecondsPerSecond;
    const auto delta_nanoseconds = scaled /
                                   static_cast<std::int64_t>(
                                       diagnostics_.anchor.sample_rate);
    std::uint64_t mapped = diagnostics_.anchor.remote_ptp_nanoseconds;
    if (delta_nanoseconds >= 0) {
        const auto magnitude = static_cast<std::uint64_t>(delta_nanoseconds);
        if (mapped > std::numeric_limits<std::uint64_t>::max() - magnitude) {
            ++diagnostics_.rejected_mappings;
            return std::nullopt;
        }
        mapped += magnitude;
    } else {
        const auto magnitude =
            static_cast<std::uint64_t>(-(delta_nanoseconds + 1LL)) + 1U;
        if (magnitude > mapped) {
            ++diagnostics_.rejected_mappings;
            return std::nullopt;
        }
        mapped -= magnitude;
    }
    if (mapped == 0U) {
        ++diagnostics_.rejected_mappings;
        return std::nullopt;
    }
    ++diagnostics_.mapped_timestamps;
    return mapped;
}

std::optional<std::uint32_t> RtpPtpPhaseTimeline::RemotePtpNanosecondsToRtp(
    const std::uint64_t remote_ptp_nanoseconds) const noexcept {
    std::scoped_lock lock{mutex_};
    if (!diagnostics_.anchored) {
        ++diagnostics_.rejected_mappings;
        return std::nullopt;
    }
    const auto delta_nanoseconds = remote_ptp_nanoseconds >=
                                           diagnostics_.anchor.remote_ptp_nanoseconds
                                       ? static_cast<long double>(
                                             remote_ptp_nanoseconds -
                                             diagnostics_.anchor.remote_ptp_nanoseconds)
                                       : -static_cast<long double>(
                                             diagnostics_.anchor.remote_ptp_nanoseconds -
                                             remote_ptp_nanoseconds);
    const auto frames = delta_nanoseconds * diagnostics_.anchor.sample_rate /
                        1'000'000'000.0L;
    if (frames > static_cast<long double>(std::numeric_limits<std::int32_t>::max()) ||
        frames < static_cast<long double>(std::numeric_limits<std::int32_t>::min())) {
        ++diagnostics_.rejected_mappings;
        return std::nullopt;
    }
    ++diagnostics_.mapped_timestamps;
    return diagnostics_.anchor.rtp_timestamp +
           static_cast<std::uint32_t>(static_cast<std::int32_t>(std::llround(frames)));
}

RtpPtpPhaseDiagnostics RtpPtpPhaseTimeline::Diagnostics() const noexcept {
    std::scoped_lock lock{mutex_};
    return diagnostics_;
}

}  // namespace airplaywin::timing
