#include "TestFramework.h"

#include <vector>

#include "core/audio/LoopbackLatencyAnalyzer.h"

void TestLoopbackLatency() {
    using airplaywin::audio::LoopbackLatencyAnalyzer;
    using airplaywin::audio::LoopbackLatencyConfig;
    using airplaywin::audio::LoopbackLatencyStatus;

    std::vector<float> stereo(4'800U * 2U, 0.01F);
    stereo[480U * 2U] = 0.8F;
    stereo[960U * 2U + 1U] = -0.7F;
    const auto dual_channel = LoopbackLatencyAnalyzer::Analyze(
        stereo, LoopbackLatencyConfig{});
    APW_EXPECT(dual_channel.Succeeded());
    APW_EXPECT(dual_channel.reference_onset_frame == 480U);
    APW_EXPECT(dual_channel.output_onset_frame == 960U);
    APW_EXPECT(dual_channel.latency_frames == 480U);
    APW_EXPECT(dual_channel.latency_microseconds == 10'000U);

    std::vector<float> mono(2'000U, 0.0F);
    mono[720U] = 0.9F;
    const auto known_stimulus = LoopbackLatencyAnalyzer::Analyze(
        mono, LoopbackLatencyConfig{
                  .sample_rate = 48'000U,
                  .channel_count = 1U,
                  .reference_channel = {},
                  .known_stimulus_frame = 480U,
                  .output_channel = 0U,
              });
    APW_EXPECT(known_stimulus.Succeeded());
    APW_EXPECT(known_stimulus.latency_frames == 240U);
    APW_EXPECT(known_stimulus.latency_microseconds == 5'000U);

    const auto invalid = LoopbackLatencyAnalyzer::Analyze(
        stereo, LoopbackLatencyConfig{
                    .reference_channel = std::uint16_t{1U}, .output_channel = 1U});
    APW_EXPECT(invalid.status == LoopbackLatencyStatus::InvalidConfiguration);

    std::vector<float> silence(4'800U * 2U, 0.0F);
    silence[100U * 2U] = 0.8F;
    const auto missing_output = LoopbackLatencyAnalyzer::Analyze(
        silence, LoopbackLatencyConfig{});
    APW_EXPECT(missing_output.status == LoopbackLatencyStatus::OutputOnsetNotFound);

    mono.assign(2'000U, 0.0F);
    mono[1'200U] = 0.9F;
    const auto out_of_range = LoopbackLatencyAnalyzer::Analyze(
        mono, LoopbackLatencyConfig{
                  .sample_rate = 48'000U,
                  .channel_count = 1U,
                  .reference_channel = {},
                  .known_stimulus_frame = 480U,
                  .output_channel = 0U,
                  .maximum_latency_milliseconds = 10U,
              });
    APW_EXPECT(out_of_range.status == LoopbackLatencyStatus::LatencyOutOfRange);
}
