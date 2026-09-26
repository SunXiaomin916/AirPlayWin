#include "TestFramework.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include "core/audio/DriftResampler.h"

void TestDriftResampler() {
    airplaywin::audio::DriftResampler resampler;
    std::vector<float> input(200U);
    for (std::size_t index = 0U; index < input.size(); ++index) {
        input[index] = static_cast<float>(index) / static_cast<float>(input.size());
    }
    std::vector<float> output(220U);
    auto result = resampler.Process(input, 100U, 2U, 1.0, output);
    APW_EXPECT(result.succeeded);
    APW_EXPECT(result.output_frames == 100U);
    for (std::size_t index = 0U; index < input.size(); ++index) {
        APW_EXPECT(input[index] == output[index]);
    }

    resampler.Reset();
    std::uint64_t slow_frames = 0U;
    for (std::uint32_t packet = 0U; packet < 1'000U; ++packet) {
        result = resampler.Process(input, 100U, 2U, 0.9999, output);
        APW_EXPECT(result.succeeded);
        slow_frames += result.output_frames;
    }
    APW_EXPECT(slow_frames == 99'990U);

    resampler.Reset();
    std::uint64_t fast_frames = 0U;
    for (std::uint32_t packet = 0U; packet < 1'000U; ++packet) {
        result = resampler.Process(input, 100U, 2U, 1.0001, output);
        APW_EXPECT(result.succeeded);
        fast_frames += result.output_frames;
        for (std::size_t index = 0U;
             index < static_cast<std::size_t>(result.output_frames) * 2U; ++index) {
            APW_EXPECT(std::isfinite(output[index]));
        }
    }
    APW_EXPECT(fast_frames == 100'010U);
    APW_EXPECT(!resampler.Process(input, 100U, 2U, 1.1, output).succeeded);
}
