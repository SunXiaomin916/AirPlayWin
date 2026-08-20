#pragma once

#include <cstdint>

namespace airplaywin::windows::audio {

struct EndpointLatencyInput final {
    std::uint32_t sample_rate{0U};
    std::uint32_t software_queue_frames{0U};
    std::uint32_t endpoint_padding_frames{0U};
    std::uint32_t engine_period_frames{0U};
    std::uint64_t engine_latency_microseconds{0U};
    std::int64_t calibration_offset_microseconds{0};
};

struct EndpointLatencyEstimate final {
    std::uint64_t software_queue_microseconds{0U};
    std::uint64_t endpoint_padding_microseconds{0U};
    std::uint64_t engine_microseconds{0U};
    std::int64_t calibration_offset_microseconds{0};
    std::uint64_t total_microseconds{0U};
};

class EndpointLatencyModel final {
public:
    [[nodiscard]] static EndpointLatencyEstimate Estimate(
        const EndpointLatencyInput& input) noexcept;
};

}  // namespace airplaywin::windows::audio
