#include "core/audio/TestSignalGenerator.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace airplaywin::audio {

TestSignalGenerator::TestSignalGenerator(const AudioFormat format,
                                         const TestSignal signal,
                                         const float amplitude)
    : format_(format), signal_(signal), amplitude_(std::clamp(amplitude, 0.0F, 1.0F)) {
    if (!format.IsValid() || !std::isfinite(amplitude)) {
        throw std::invalid_argument("Invalid test signal configuration");
    }
}

void TestSignalGenerator::Fill(const std::span<float> interleaved_destination,
                               const std::uint32_t frame_count) noexcept {
    const auto channels = static_cast<std::size_t>(format_.channel_count);
    const auto required_samples = static_cast<std::size_t>(frame_count) * channels;
    if (interleaved_destination.size() < required_samples) {
        return;
    }

    for (std::uint32_t frame = 0U; frame < frame_count; ++frame) {
        float sample = 0.0F;
        if (signal_ == TestSignal::Impulse) {
            sample = (generated_frames_ % format_.sample_rate == 0U) ? amplitude_ : 0.0F;
        } else if (signal_ == TestSignal::LatencyPulse) {
            const auto initial_silence_frames = format_.sample_rate / 4U;
            sample = generated_frames_ >= initial_silence_frames &&
                             (generated_frames_ - initial_silence_frames) %
                                     format_.sample_rate ==
                                 0U
                         ? amplitude_
                         : 0.0F;
        } else if (signal_ != TestSignal::Silence) {
            sample = amplitude_ * static_cast<float>(std::sin(phase_radians_));
            const auto phase_step = 2.0 * std::numbers::pi_v<double> *
                                    FrequencyForCurrentFrame() /
                                    static_cast<double>(format_.sample_rate);
            phase_radians_ = std::fmod(phase_radians_ + phase_step,
                                      2.0 * std::numbers::pi_v<double>);
        }

        for (std::size_t channel = 0U; channel < channels; ++channel) {
            interleaved_destination[static_cast<std::size_t>(frame) * channels + channel] = sample;
        }
        ++generated_frames_;
    }
}

void TestSignalGenerator::Reset() noexcept {
    phase_radians_ = 0.0;
    generated_frames_ = 0U;
}

double TestSignalGenerator::FrequencyForCurrentFrame() const noexcept {
    if (signal_ == TestSignal::Sine440Hz) {
        return 440.0;
    }
    if (signal_ == TestSignal::Sine1kHz) {
        return 1'000.0;
    }
    if (signal_ == TestSignal::Sweep) {
        constexpr double kStartFrequency = 20.0;
        constexpr double kEndFrequency = 20'000.0;
        const auto sweep_frames = static_cast<std::uint64_t>(format_.sample_rate) * 10U;
        const auto position = static_cast<double>(generated_frames_ % sweep_frames) /
                              static_cast<double>(sweep_frames);
        return kStartFrequency * std::pow(kEndFrequency / kStartFrequency, position);
    }
    return 0.0;
}

}  // namespace airplaywin::audio
