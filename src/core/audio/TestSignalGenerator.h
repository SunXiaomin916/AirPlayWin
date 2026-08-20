#pragma once

#include <cstdint>
#include <span>

#include "core/audio/AudioTypes.h"

namespace airplaywin::audio {

enum class TestSignal : std::uint8_t {
    Sine440Hz,
    Sine1kHz,
    Silence,
    Impulse,
    Sweep,
};

class TestSignalGenerator final {
public:
    TestSignalGenerator(AudioFormat format, TestSignal signal, float amplitude = 0.2F);

    void Fill(std::span<float> interleaved_destination, std::uint32_t frame_count) noexcept;
    void Reset() noexcept;

private:
    [[nodiscard]] double FrequencyForCurrentFrame() const noexcept;

    AudioFormat format_;
    TestSignal signal_;
    float amplitude_;
    double phase_radians_{0.0};
    std::uint64_t generated_frames_{0U};
};

}  // namespace airplaywin::audio
