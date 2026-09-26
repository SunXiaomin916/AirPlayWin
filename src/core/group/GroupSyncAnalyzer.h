#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/group/GroupTypes.h"

namespace airplaywin::group {

enum class GroupSyncAnalysisStatus : std::uint8_t {
    Ok,
    InvalidConfiguration,
    NoReferencePulses,
    InsufficientCompletePulses,
};

struct GroupSyncTargets final {
    std::uint64_t p95_microseconds{1'000U};
    std::uint64_t p99_microseconds{2'000U};
    std::uint64_t first_alignment_microseconds{5'000U};

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return p95_microseconds > 0U && p99_microseconds >= p95_microseconds &&
               first_alignment_microseconds > 0U;
    }
};

struct GroupSyncPercentiles final {
    std::uint64_t p50_microseconds{0U};
    std::uint64_t p95_microseconds{0U};
    std::uint64_t p99_microseconds{0U};
    std::uint64_t maximum_microseconds{0U};
};

struct GroupSyncMemberStatistics final {
    GroupMemberId member_id{0U};
    std::uint64_t matched_pulses{0U};
    std::int64_t mean_offset_microseconds{0};
    std::int64_t minimum_offset_microseconds{0};
    std::int64_t maximum_offset_microseconds{0};
    GroupSyncPercentiles absolute_offset{};
    double mean_correlation{0.0};
};

struct GroupSyncAnalysisResult final {
    GroupSyncAnalysisStatus status{GroupSyncAnalysisStatus::InvalidConfiguration};
    bool meets_targets{false};
    std::uint64_t reference_pulses{0U};
    std::uint64_t complete_pulses{0U};
    std::uint64_t incomplete_pulses{0U};
    std::uint64_t correlation_refinements{0U};
    std::uint64_t onset_fallbacks{0U};
    std::uint64_t first_alignment_microseconds{0U};
    GroupSyncPercentiles group_skew{};
    std::vector<GroupSyncMemberStatistics> members{};

    [[nodiscard]] constexpr bool Succeeded() const noexcept {
        return status == GroupSyncAnalysisStatus::Ok;
    }
};

struct GroupSyncWaveformMember final {
    GroupMemberId member_id{0U};
    std::uint16_t channel{0U};
};

struct GroupSyncWaveformConfig final {
    std::uint32_t sample_rate{48'000U};
    std::uint16_t channel_count{2U};
    std::vector<GroupSyncWaveformMember> members{};
    float onset_threshold{0.25F};
    std::uint32_t minimum_pulse_gap_milliseconds{100U};
    std::uint32_t maximum_absolute_skew_milliseconds{100U};
    std::uint32_t correlation_window_frames{96U};
    std::uint32_t correlation_search_frames{16U};
    double minimum_correlation{0.20};
    std::uint32_t minimum_complete_pulses{1U};
    GroupSyncTargets targets{};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct GroupSyncTimestampObservation final {
    std::uint64_t pulse_index{0U};
    GroupMemberId member_id{0U};
    std::int64_t presentation_time_nanoseconds{0};
};

struct GroupSyncTimestampConfig final {
    std::vector<GroupMemberId> members{};
    std::uint32_t minimum_complete_pulses{1U};
    GroupSyncTargets targets{};

    [[nodiscard]] bool IsValid() const noexcept;
};

class GroupSyncAnalyzer final {
public:
    [[nodiscard]] static GroupSyncAnalysisResult AnalyzeWaveform(
        std::span<const float> interleaved_capture,
        const GroupSyncWaveformConfig& config);

    [[nodiscard]] static GroupSyncAnalysisResult AnalyzeTimestamps(
        std::span<const GroupSyncTimestampObservation> observations,
        const GroupSyncTimestampConfig& config);
};

}  // namespace airplaywin::group
