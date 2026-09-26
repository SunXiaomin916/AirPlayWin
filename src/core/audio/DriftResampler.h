#pragma once

#include <cstdint>
#include <span>

namespace airplaywin::audio {

struct DriftResampleResult final {
    bool succeeded{false};
    std::uint32_t output_frames{0U};
};

// Bounded asynchronous correction for clock-servo ratios near unity. The caller owns
// preallocated storage, so processing performs no allocation and is safe on the stream worker.
class DriftResampler final {
public:
    [[nodiscard]] DriftResampleResult Process(
        std::span<const float> interleaved_input,
        std::uint32_t input_frames,
        std::uint16_t channel_count,
        double output_to_input_ratio,
        std::span<float> interleaved_output) noexcept;
    void Reset() noexcept;

private:
    long double fractional_output_frames_{0.0L};
};

}  // namespace airplaywin::audio
