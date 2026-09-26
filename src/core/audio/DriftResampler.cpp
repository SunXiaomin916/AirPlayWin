#include "core/audio/DriftResampler.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace airplaywin::audio {

DriftResampleResult DriftResampler::Process(
    const std::span<const float> interleaved_input,
    const std::uint32_t input_frames,
    const std::uint16_t channel_count,
    const double output_to_input_ratio,
    const std::span<float> interleaved_output) noexcept {
    if (input_frames == 0U || channel_count == 0U || channel_count > 8U ||
        interleaved_input.size() !=
            static_cast<std::size_t>(input_frames) * channel_count ||
        !std::isfinite(output_to_input_ratio) || output_to_input_ratio < 0.98 ||
        output_to_input_ratio > 1.02) {
        return {};
    }
    const auto desired = static_cast<long double>(input_frames) *
                             output_to_input_ratio +
                         fractional_output_frames_;
    constexpr long double kIntegerBoundaryTolerance = 1.0e-9L;
    const auto output_frames = static_cast<std::uint32_t>(
        std::floor(desired + kIntegerBoundaryTolerance));
    if (output_frames == 0U || interleaved_output.size() <
                                  static_cast<std::size_t>(output_frames) * channel_count) {
        return {};
    }
    fractional_output_frames_ = std::clamp(
        desired - output_frames, 0.0L, 1.0L - kIntegerBoundaryTolerance);

    if (output_frames == input_frames) {
        std::copy(interleaved_input.begin(), interleaved_input.end(),
                  interleaved_output.begin());
        return {.succeeded = true, .output_frames = output_frames};
    }
    const auto source_span = input_frames > 1U ? input_frames - 1U : 0U;
    const auto output_span = output_frames > 1U ? output_frames - 1U : 1U;
    for (std::uint32_t output_frame = 0U; output_frame < output_frames; ++output_frame) {
        const auto source_position = static_cast<long double>(output_frame) * source_span /
                                     output_span;
        const auto first_frame = static_cast<std::uint32_t>(std::floor(source_position));
        const auto second_frame = std::min(first_frame + 1U, input_frames - 1U);
        const auto fraction = static_cast<float>(source_position - first_frame);
        for (std::uint16_t channel = 0U; channel < channel_count; ++channel) {
            const auto first = interleaved_input[
                static_cast<std::size_t>(first_frame) * channel_count + channel];
            const auto second = interleaved_input[
                static_cast<std::size_t>(second_frame) * channel_count + channel];
            interleaved_output[static_cast<std::size_t>(output_frame) * channel_count +
                               channel] = first + (second - first) * fraction;
        }
    }
    return {.succeeded = true, .output_frames = output_frames};
}

void DriftResampler::Reset() noexcept {
    fractional_output_frames_ = 0.0L;
}

}  // namespace airplaywin::audio
