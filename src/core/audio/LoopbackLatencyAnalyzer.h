#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace airplaywin::audio {

enum class LoopbackLatencyStatus : std::uint8_t {
    Ok,
    InvalidConfiguration,
    ReferenceOnsetNotFound,
    OutputOnsetNotFound,
    LatencyOutOfRange,
};

struct LoopbackLatencyConfig final {
    std::uint32_t sample_rate{48'000U};
    std::uint16_t channel_count{2U};
    std::optional<std::uint16_t> reference_channel{std::uint16_t{0U}};
    std::optional<std::uint64_t> known_stimulus_frame{};
    std::uint16_t output_channel{1U};
    float onset_threshold{0.25F};
    std::uint32_t maximum_latency_milliseconds{1'000U};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct LoopbackLatencyResult final {
    LoopbackLatencyStatus status{LoopbackLatencyStatus::InvalidConfiguration};
    std::uint64_t reference_onset_frame{0U};
    std::uint64_t output_onset_frame{0U};
    std::uint64_t latency_frames{0U};
    std::uint64_t latency_microseconds{0U};
    float reference_peak{0.0F};
    float output_peak{0.0F};

    [[nodiscard]] constexpr bool Succeeded() const noexcept {
        return status == LoopbackLatencyStatus::Ok;
    }
};

class LoopbackLatencyAnalyzer final {
public:
    [[nodiscard]] static LoopbackLatencyResult Analyze(
        std::span<const float> interleaved_capture,
        const LoopbackLatencyConfig& config) noexcept;
};

}  // namespace airplaywin::audio
