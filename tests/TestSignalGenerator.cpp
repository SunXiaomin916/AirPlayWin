#include "TestFramework.h"

#include <array>
#include <cmath>

#include "core/audio/TestSignalGenerator.h"

void TestSignalGenerator() {
    using airplaywin::audio::AudioFormat;
    using airplaywin::audio::TestSignal;
    using airplaywin::audio::TestSignalGenerator;

    const AudioFormat format{.sample_rate = 48'000U, .channel_count = 2U};
    std::array<float, 960U> samples{};

    TestSignalGenerator sine{format, TestSignal::Sine440Hz, 0.25F};
    sine.Fill(samples, 480U);
    bool found_nonzero = false;
    for (std::size_t frame = 0U; frame < 480U; ++frame) {
        APW_EXPECT(samples[frame * 2U] == samples[frame * 2U + 1U]);
        APW_EXPECT(std::abs(samples[frame * 2U]) <= 0.25F);
        found_nonzero = found_nonzero || samples[frame * 2U] != 0.0F;
    }
    APW_EXPECT(found_nonzero);

    TestSignalGenerator silence{format, TestSignal::Silence};
    samples.fill(1.0F);
    silence.Fill(samples, 480U);
    for (const auto sample : samples) {
        APW_EXPECT(sample == 0.0F);
    }

    TestSignalGenerator impulse{format, TestSignal::Impulse, 0.5F};
    impulse.Fill(samples, 480U);
    APW_EXPECT(samples[0] == 0.5F);
    APW_EXPECT(samples[1] == 0.5F);
    APW_EXPECT(samples[2] == 0.0F);

    TestSignalGenerator latency_pulse{format, TestSignal::LatencyPulse, 0.5F};
    for (std::uint32_t block = 0U; block < 25U; ++block) {
        latency_pulse.Fill(samples, 480U);
        for (const auto sample : samples) {
            APW_EXPECT(sample == 0.0F);
        }
    }
    latency_pulse.Fill(samples, 480U);
    APW_EXPECT(samples[0] == 0.5F);
    APW_EXPECT(samples[1] == 0.5F);
    APW_EXPECT(samples[2] == 0.0F);

    TestSignalGenerator sweep{format, TestSignal::Sweep, 0.2F};
    sweep.Fill(samples, 480U);
    for (const auto sample : samples) {
        APW_EXPECT(std::isfinite(sample));
        APW_EXPECT(std::abs(sample) <= 0.2F);
    }
}
