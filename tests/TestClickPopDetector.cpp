#include "TestFramework.h"

#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <vector>

#include "core/audio/ClickPopDetector.h"

void TestClickPopDetector() {
    using airplaywin::audio::ClickPopDetector;
    using airplaywin::audio::ClickPopDetectorConfig;

    ClickPopDetector detector{
        ClickPopDetectorConfig{.maximum_sample_step = 0.25F}};
    constexpr std::uint32_t kFrames = 480U;
    std::vector<float> sine(static_cast<std::size_t>(kFrames) * 2U);
    for (std::uint32_t frame = 0U; frame < kFrames; ++frame) {
        const auto phase = 2.0 * std::numbers::pi * 440.0 *
                           static_cast<double>(frame) / 48'000.0;
        const auto sample = static_cast<float>(0.5 * std::sin(phase));
        sine[static_cast<std::size_t>(frame) * 2U] = sample;
        sine[static_cast<std::size_t>(frame) * 2U + 1U] = sample;
    }
    detector.Process(sine, kFrames, 2U);
    auto diagnostics = detector.Diagnostics();
    APW_EXPECT(diagnostics.analyzed_frames == kFrames);
    APW_EXPECT(diagnostics.transient_events == 0U);
    APW_EXPECT(diagnostics.maximum_sample_step < 0.04F);

    detector.Reset();
    std::array<float, 16U> discontinuity{};
    for (std::size_t index = 8U; index < discontinuity.size(); ++index) {
        discontinuity[index] = 1.0F;
    }
    detector.Process(discontinuity, 8U, 2U);
    diagnostics = detector.Diagnostics();
    APW_EXPECT(diagnostics.analyzed_frames == 8U);
    APW_EXPECT(diagnostics.transient_events == 1U);
    APW_EXPECT(diagnostics.last_event_frame == 4U);
    APW_EXPECT_NEAR(diagnostics.maximum_sample_step, 1.0, 0.0001);
    APW_EXPECT_NEAR(diagnostics.recent_peak_step, 1.0, 0.0001);

    bool rejected_invalid_threshold = false;
    try {
        static_cast<void>(ClickPopDetector{
            ClickPopDetectorConfig{.maximum_sample_step = 0.0F}});
    } catch (const std::invalid_argument&) {
        rejected_invalid_threshold = true;
    }
    APW_EXPECT(rejected_invalid_threshold);
}
