#include "TestFramework.h"

#include <array>
#include <cmath>
#include <limits>

#include "core/audio/AudioTransitionGuard.h"

void TestAudioTransitionGuard() {
    using airplaywin::audio::AudioTransition;
    using airplaywin::audio::AudioTransitionConfig;
    using airplaywin::audio::AudioTransitionGuard;
    using airplaywin::audio::AudioTransitionState;

    AudioTransitionGuard guard{AudioTransitionConfig{
        .sample_rate = 1'000U,
        .fade_in_milliseconds = 5.0F,
        .fade_out_milliseconds = 5.0F,
        .volume_ramp_milliseconds = 5.0F,
        .prewarm_milliseconds = 2.0F,
        .maximum_pcm_amplitude = 1.0F,
        .dc_offset_threshold = 0.5F,
    }};

    guard.Request(AudioTransition::Start);
    std::array<float, 20U> samples{};
    samples.fill(1.0F);
    guard.Process(samples, 10U, 2U, false);
    APW_EXPECT(samples[0] == 0.0F);
    APW_EXPECT(samples[1] == 0.0F);
    APW_EXPECT(guard.State() == AudioTransitionState::Audible);
    APW_EXPECT(samples.back() > 0.99F);

    guard.SetTargetVolume(0.0F);
    samples.fill(1.0F);
    guard.Process(samples, 5U, 2U, false);
    APW_EXPECT(samples[0] < 1.0F);
    APW_EXPECT_NEAR(samples[9], 0.0, 0.0001);

    guard.SetTargetVolume(1.0F);
    samples.fill(1.0F);
    guard.Process(samples, 5U, 2U, false);
    samples[0] = std::numeric_limits<float>::quiet_NaN();
    samples[1] = std::numeric_limits<float>::infinity();
    samples[2] = 5.0F;
    guard.Process(samples, 10U, 2U, false);
    APW_EXPECT(std::isfinite(samples[0]));
    APW_EXPECT(std::isfinite(samples[1]));
    APW_EXPECT(guard.InvalidNumericSamples() >= 2U);
    APW_EXPECT(guard.ClippedSamples() >= 1U);

    samples.fill(0.0F);
    guard.Process(samples, 10U, 2U, true);
    APW_EXPECT(guard.State() == AudioTransitionState::SafeMute);

    guard.Request(AudioTransition::HardResync);
    std::array<float, 40U> resync_samples{};
    resync_samples.fill(1.0F);
    guard.Process(resync_samples, 20U, 2U, false);
    APW_EXPECT(guard.State() == AudioTransitionState::SafeMute);
    guard.Request(AudioTransition::Resume);
    resync_samples.fill(1.0F);
    guard.Process(resync_samples, 20U, 2U, false);
    APW_EXPECT(guard.State() == AudioTransitionState::Audible);
    for (const auto sample : resync_samples) {
        APW_EXPECT(std::isfinite(sample));
        APW_EXPECT(std::abs(sample) <= 1.0F);
    }

    AudioTransitionGuard stress_guard{AudioTransitionConfig{
        .sample_rate = 1'000U,
        .fade_in_milliseconds = 5.0F,
        .fade_out_milliseconds = 5.0F,
        .volume_ramp_milliseconds = 5.0F,
        .prewarm_milliseconds = 2.0F,
        .maximum_pcm_amplitude = 1.0F,
        .dc_offset_threshold = 0.5F,
    }};
    constexpr std::array<AudioTransition, 6U> kStressTransitions{
        AudioTransition::Stop,       AudioTransition::Pause, AudioTransition::Flush,
        AudioTransition::Seek,       AudioTransition::HardResync,
        AudioTransition::DeviceSwitch};
    std::array<float, 40U> stress_samples{};
    float previous_sample = 0.0F;
    const auto process_stress_block = [&]() {
        stress_samples.fill(0.5F);
        stress_guard.Process(stress_samples, 20U, 2U, false);
        for (const auto output : stress_samples) {
            APW_EXPECT(std::isfinite(output));
            APW_EXPECT(std::abs(output - previous_sample) <= 0.25F);
            previous_sample = output;
        }
    };

    stress_guard.Request(AudioTransition::Start);
    process_stress_block();
    for (std::size_t cycle = 0U; cycle < 1'000U; ++cycle) {
        stress_guard.Request(kStressTransitions[cycle % kStressTransitions.size()]);
        process_stress_block();
        if (stress_guard.State() != AudioTransitionState::Audible) {
            stress_guard.Request(AudioTransition::Start);
            process_stress_block();
        }
    }
}
