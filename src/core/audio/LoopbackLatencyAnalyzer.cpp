#include "core/audio/LoopbackLatencyAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace airplaywin::audio {
namespace {

struct Onset final {
    bool found{false};
    std::uint64_t frame{0U};
    float peak{0.0F};
};

[[nodiscard]] Onset FindOnset(const std::span<const float> capture,
                              const std::uint16_t channels,
                              const std::uint16_t channel,
                              const std::uint64_t first_frame,
                              const std::uint64_t frame_count,
                              const float threshold) noexcept {
    Onset onset;
    const auto last_frame = std::min<std::uint64_t>(
        frame_count, static_cast<std::uint64_t>(capture.size() / channels));
    for (auto frame = first_frame; frame < last_frame; ++frame) {
        const auto sample = capture[static_cast<std::size_t>(frame) * channels + channel];
        const auto magnitude = std::isfinite(sample) ? std::abs(sample) : 0.0F;
        onset.peak = std::max(onset.peak, magnitude);
        if (!onset.found && magnitude >= threshold) {
            onset.found = true;
            onset.frame = frame;
        }
    }
    return onset;
}

}  // namespace

bool LoopbackLatencyConfig::IsValid() const noexcept {
    const bool has_reference = reference_channel.has_value();
    const bool has_known_frame = known_stimulus_frame.has_value();
    return sample_rate >= 8'000U && sample_rate <= 384'000U && channel_count >= 1U &&
           channel_count <= 8U && output_channel < channel_count &&
           has_reference != has_known_frame &&
           (!has_reference || (*reference_channel < channel_count &&
                               *reference_channel != output_channel)) &&
           std::isfinite(onset_threshold) && onset_threshold > 0.0F &&
           onset_threshold <= 1.0F && maximum_latency_milliseconds >= 1U &&
           maximum_latency_milliseconds <= 10'000U;
}

LoopbackLatencyResult LoopbackLatencyAnalyzer::Analyze(
    const std::span<const float> interleaved_capture,
    const LoopbackLatencyConfig& config) noexcept {
    if (!config.IsValid() || interleaved_capture.empty() ||
        interleaved_capture.size() % config.channel_count != 0U) {
        return {.status = LoopbackLatencyStatus::InvalidConfiguration};
    }
    const auto frame_count = static_cast<std::uint64_t>(
        interleaved_capture.size() / config.channel_count);
    std::uint64_t reference_frame = 0U;
    float reference_peak = 1.0F;
    if (config.reference_channel.has_value()) {
        const auto reference = FindOnset(interleaved_capture, config.channel_count,
                                         *config.reference_channel, 0U, frame_count,
                                         config.onset_threshold);
        if (!reference.found) {
            return {.status = LoopbackLatencyStatus::ReferenceOnsetNotFound,
                    .reference_peak = reference.peak};
        }
        reference_frame = reference.frame;
        reference_peak = reference.peak;
    } else {
        reference_frame = *config.known_stimulus_frame;
        if (reference_frame >= frame_count) {
            return {.status = LoopbackLatencyStatus::InvalidConfiguration};
        }
    }

    const auto maximum_latency_frames =
        static_cast<std::uint64_t>(config.sample_rate) *
        config.maximum_latency_milliseconds / 1'000U;
    const auto output = FindOnset(interleaved_capture, config.channel_count,
                                  config.output_channel, reference_frame, frame_count,
                                  config.onset_threshold);
    if (!output.found) {
        return {.status = LoopbackLatencyStatus::OutputOnsetNotFound,
                .reference_onset_frame = reference_frame,
                .reference_peak = reference_peak,
                .output_peak = output.peak};
    }
    const auto latency_frames = output.frame - reference_frame;
    if (latency_frames > maximum_latency_frames) {
        return {.status = LoopbackLatencyStatus::LatencyOutOfRange,
                .reference_onset_frame = reference_frame,
                .output_onset_frame = output.frame,
                .latency_frames = latency_frames,
                .reference_peak = reference_peak,
                .output_peak = output.peak};
    }
    return {
        .status = LoopbackLatencyStatus::Ok,
        .reference_onset_frame = reference_frame,
        .output_onset_frame = output.frame,
        .latency_frames = latency_frames,
        .latency_microseconds = latency_frames * 1'000'000U / config.sample_rate,
        .reference_peak = reference_peak,
        .output_peak = output.peak,
    };
}

}  // namespace airplaywin::audio
