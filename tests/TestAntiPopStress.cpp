#include "TestFramework.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

#include "core/audio/AudioTransitionGuard.h"

void TestAntiPopStress() {
    using airplaywin::audio::AudioTransition;
    using airplaywin::audio::AudioTransitionConfig;
    using airplaywin::audio::AudioTransitionGuard;
    using airplaywin::audio::AudioTransitionState;

    constexpr std::uint32_t kSampleRate = 48'000U;
    constexpr std::uint32_t kBlockFrames = 240U;
    constexpr std::uint16_t kChannels = 2U;
    constexpr double kPhaseStep =
        2.0 * std::numbers::pi * 440.0 / static_cast<double>(kSampleRate);
    AudioTransitionGuard guard{AudioTransitionConfig{
        .sample_rate = kSampleRate,
        .fade_in_milliseconds = 10.0F,
        .fade_out_milliseconds = 10.0F,
        .volume_ramp_milliseconds = 10.0F,
        .prewarm_milliseconds = 5.0F,
        .maximum_pcm_amplitude = 1.0F,
        .dc_offset_threshold = 0.5F,
        .click_pop_step_threshold = 0.20F,
    }};
    std::array<float, static_cast<std::size_t>(kBlockFrames) * kChannels> block{};
    double phase = 0.0;

    const auto process_block = [&](const bool underrun) {
        for (std::uint32_t frame = 0U; frame < kBlockFrames; ++frame) {
            const auto sample = underrun ? 0.0F
                                         : static_cast<float>(0.6 * std::sin(phase));
            block[static_cast<std::size_t>(frame) * kChannels] = sample;
            block[static_cast<std::size_t>(frame) * kChannels + 1U] = sample;
            phase += kPhaseStep;
            if (phase >= 2.0 * std::numbers::pi) {
                phase -= 2.0 * std::numbers::pi;
            }
        }
        guard.Process(block, kBlockFrames, kChannels, underrun);
    };
    const auto process_blocks = [&](const std::uint32_t count, const bool underrun = false) {
        for (std::uint32_t block_index = 0U; block_index < count; ++block_index) {
            process_block(underrun);
        }
    };

    guard.Request(AudioTransition::Start);
    process_blocks(3U);
    APW_EXPECT(guard.State() == AudioTransitionState::Audible);

    constexpr std::array<AudioTransition, 7U> kTransitions{
        AudioTransition::Stop,       AudioTransition::Pause,
        AudioTransition::Flush,      AudioTransition::Seek,
        AudioTransition::HardResync, AudioTransition::DeviceSwitch,
        AudioTransition::None,
    };
    for (std::uint32_t cycle = 0U; cycle < 1'000U; ++cycle) {
        process_block(false);
        guard.SetTargetVolume((cycle % 2U) == 0U ? 0.0F : 1.0F);
        process_blocks(2U);

        const auto transition = kTransitions[cycle % kTransitions.size()];
        if (transition == AudioTransition::None) {
            process_blocks(2U, true);
        } else {
            guard.Request(transition);
            process_blocks(2U);
        }
        APW_EXPECT(guard.State() != AudioTransitionState::Audible);

        if (transition == AudioTransition::Flush || transition == AudioTransition::Seek ||
            transition == AudioTransition::HardResync) {
            phase = std::fmod(static_cast<double>(cycle + 1U) * 0.731,
                              2.0 * std::numbers::pi);
        }
        guard.Request(transition == AudioTransition::Pause ? AudioTransition::Resume
                                                           : AudioTransition::Start);
        process_blocks(3U);
        APW_EXPECT(guard.State() == AudioTransitionState::Audible);
    }

    const auto click_pop = guard.ClickPop();
    APW_EXPECT(click_pop.analyzed_frames == 1'920'720U);
    APW_EXPECT(click_pop.transient_events == 0U);
    APW_EXPECT(click_pop.maximum_sample_step < 0.20F);
    APW_EXPECT(guard.TransitionRequests() >= 1'850U);
    APW_EXPECT(guard.FadeInEvents() == 1'001U);
    APW_EXPECT(guard.FadeOutEvents() == 1'000U);
    APW_EXPECT(guard.SafeMuteEvents() >= 570U);
    APW_EXPECT(guard.HardResyncEvents() >= 142U);
    APW_EXPECT(guard.UnderrunTransitionEvents() >= 142U);
}
