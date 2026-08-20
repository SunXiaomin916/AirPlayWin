#include "platform/windows/audio/EndpointLatencyModel.h"

#include <algorithm>
#include <limits>

namespace airplaywin::windows::audio {
namespace {

[[nodiscard]] std::uint64_t FramesToMicroseconds(const std::uint32_t frames,
                                                 const std::uint32_t sample_rate) noexcept {
    if (sample_rate == 0U) {
        return 0U;
    }
    return static_cast<std::uint64_t>(frames) * 1'000'000U / sample_rate;
}

[[nodiscard]] std::uint64_t SaturatingAdd(const std::uint64_t left,
                                          const std::uint64_t right) noexcept {
    return right > std::numeric_limits<std::uint64_t>::max() - left
               ? std::numeric_limits<std::uint64_t>::max()
               : left + right;
}

}  // namespace

EndpointLatencyEstimate EndpointLatencyModel::Estimate(
    const EndpointLatencyInput& input) noexcept {
    EndpointLatencyEstimate estimate{
        .software_queue_microseconds =
            FramesToMicroseconds(input.software_queue_frames, input.sample_rate),
        .endpoint_padding_microseconds =
            FramesToMicroseconds(input.endpoint_padding_frames, input.sample_rate),
        .engine_microseconds = input.engine_latency_microseconds,
        .calibration_offset_microseconds = input.calibration_offset_microseconds,
    };
    if (estimate.engine_microseconds == 0U) {
        estimate.engine_microseconds =
            FramesToMicroseconds(input.engine_period_frames, input.sample_rate);
    }
    auto total = SaturatingAdd(estimate.software_queue_microseconds,
                               estimate.endpoint_padding_microseconds);
    total = SaturatingAdd(total, estimate.engine_microseconds);
    if (estimate.calibration_offset_microseconds < 0) {
        const auto magnitude = static_cast<std::uint64_t>(
            -(estimate.calibration_offset_microseconds + 1)) + 1U;
        estimate.total_microseconds = magnitude >= total ? 0U : total - magnitude;
    } else {
        estimate.total_microseconds = SaturatingAdd(
            total, static_cast<std::uint64_t>(estimate.calibration_offset_microseconds));
    }
    return estimate;
}

}  // namespace airplaywin::windows::audio
