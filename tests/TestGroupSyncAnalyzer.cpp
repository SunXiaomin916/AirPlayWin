#include "TestFramework.h"

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include "core/group/GroupSyncAnalyzer.h"

void TestGroupSyncAnalyzer() {
    using airplaywin::group::GroupSyncAnalysisStatus;
    using airplaywin::group::GroupSyncAnalyzer;
    using airplaywin::group::GroupSyncTimestampConfig;
    using airplaywin::group::GroupSyncTimestampObservation;
    using airplaywin::group::GroupSyncWaveformConfig;
    using airplaywin::group::GroupSyncWaveformMember;

    constexpr std::uint32_t sample_rate = 48'000U;
    constexpr std::uint16_t channels = 4U;
    constexpr std::uint32_t pulse_count = 100U;
    constexpr std::uint32_t pulse_spacing = 1'000U;
    constexpr std::uint32_t pulse_length = 64U;
    std::vector<float> capture(
        static_cast<std::size_t>(pulse_count * pulse_spacing + 1'000U) * channels,
        0.001F);
    for (std::uint32_t pulse = 0U; pulse < pulse_count; ++pulse) {
        const std::int32_t offsets[channels]{
            0,
            12 + static_cast<std::int32_t>(pulse % 3U),
            -10 + static_cast<std::int32_t>(pulse % 5U),
            24 + static_cast<std::int32_t>(pulse % 7U),
        };
        for (std::uint16_t channel = 0U; channel < channels; ++channel) {
            if (channel == 3U && pulse == 50U) {
                continue;
            }
            const auto start = static_cast<std::int64_t>(500U + pulse * pulse_spacing) +
                               offsets[channel];
            for (std::uint32_t frame = 0U; frame < pulse_length; ++frame) {
                const auto shape = static_cast<float>(
                    0.8 * std::sin(std::numbers::pi * (frame + 1U) /
                                   (pulse_length + 1U)));
                capture[(static_cast<std::size_t>(start) + frame) * channels + channel] =
                    shape;
            }
        }
    }

    GroupSyncWaveformConfig config{
        .sample_rate = sample_rate,
        .channel_count = channels,
        .members = {GroupSyncWaveformMember{.member_id = 11U, .channel = 0U},
                    GroupSyncWaveformMember{.member_id = 12U, .channel = 1U},
                    GroupSyncWaveformMember{.member_id = 13U, .channel = 2U},
                    GroupSyncWaveformMember{.member_id = 14U, .channel = 3U}},
        .onset_threshold = 0.25F,
        .minimum_pulse_gap_milliseconds = 10U,
        .maximum_absolute_skew_milliseconds = 10U,
        .correlation_window_frames = 96U,
        .correlation_search_frames = 16U,
        .minimum_correlation = 0.8,
        .minimum_complete_pulses = 90U,
    };
    const auto waveform = GroupSyncAnalyzer::AnalyzeWaveform(capture, config);
    APW_EXPECT(waveform.status == GroupSyncAnalysisStatus::Ok);
    APW_EXPECT(waveform.reference_pulses == pulse_count);
    APW_EXPECT(waveform.complete_pulses == pulse_count - 1U);
    APW_EXPECT(waveform.incomplete_pulses == 1U);
    APW_EXPECT(waveform.correlation_refinements ==
               (pulse_count - 1U) * (channels - 1U));
    APW_EXPECT(waveform.onset_fallbacks == 0U);
    APW_EXPECT(waveform.members.size() == channels);
    APW_EXPECT(waveform.group_skew.p95_microseconds <= 1'000U);
    APW_EXPECT(waveform.group_skew.p99_microseconds <= 2'000U);
    APW_EXPECT(waveform.first_alignment_microseconds <= 5'000U);
    APW_EXPECT(waveform.meets_targets);
    APW_EXPECT(waveform.members[1U].mean_offset_microseconds > 200);
    APW_EXPECT(waveform.members[2U].mean_offset_microseconds < 0);

    std::vector<GroupSyncTimestampObservation> timestamps;
    for (std::uint64_t pulse = 0U; pulse < 100U; ++pulse) {
        const auto base = static_cast<std::int64_t>(pulse * 1'000'000'000ULL);
        timestamps.push_back({.pulse_index = pulse,
                              .member_id = 1U,
                              .presentation_time_nanoseconds = base});
        timestamps.push_back({.pulse_index = pulse,
                              .member_id = 2U,
                              .presentation_time_nanoseconds = base + 500'000});
        timestamps.push_back({.pulse_index = pulse,
                              .member_id = 3U,
                              .presentation_time_nanoseconds = base + 1'200'000});
    }
    const auto timestamp_result = GroupSyncAnalyzer::AnalyzeTimestamps(
        timestamps, GroupSyncTimestampConfig{.members = {1U, 2U, 3U},
                                              .minimum_complete_pulses = 100U});
    APW_EXPECT(timestamp_result.Succeeded());
    APW_EXPECT(timestamp_result.group_skew.p50_microseconds == 1'200U);
    APW_EXPECT(timestamp_result.group_skew.p95_microseconds == 1'200U);
    APW_EXPECT(!timestamp_result.meets_targets);

    timestamps.push_back(timestamps.back());
    APW_EXPECT(GroupSyncAnalyzer::AnalyzeTimestamps(
                   timestamps,
                   GroupSyncTimestampConfig{.members = {1U, 2U, 3U}})
                   .status == GroupSyncAnalysisStatus::InvalidConfiguration);
    APW_EXPECT(!GroupSyncWaveformConfig{}.IsValid());
}
