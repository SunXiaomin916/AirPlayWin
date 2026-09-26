#include "core/group/GroupSyncAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace airplaywin::group {
namespace {

struct PulseMeasurement final {
    std::vector<std::int64_t> offsets_microseconds{};
    std::vector<double> correlations{};
    std::vector<bool> correlation_refined{};
};

struct RefinedLag final {
    std::int64_t frames{0};
    double correlation{0.0};
    bool refined{false};
};

[[nodiscard]] bool HasUniqueMembers(
    const std::span<const GroupMemberId> members) noexcept {
    std::unordered_set<GroupMemberId> unique;
    unique.reserve(members.size());
    for (const auto member : members) {
        if (member == 0U || !unique.insert(member).second) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::uint64_t AbsoluteMicroseconds(const std::int64_t value) noexcept {
    if (value >= 0) {
        return static_cast<std::uint64_t>(value);
    }
    if (value == std::numeric_limits<std::int64_t>::min()) {
        return static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U;
    }
    return static_cast<std::uint64_t>(-value);
}

[[nodiscard]] std::int64_t NanosecondsToMicroseconds(
    const long double nanoseconds) noexcept {
    const auto microseconds = nanoseconds / 1'000.0L;
    if (microseconds >=
        static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (microseconds <=
        static_cast<long double>(std::numeric_limits<std::int64_t>::min())) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(std::llround(microseconds));
}

[[nodiscard]] std::uint64_t FramesToMicroseconds(
    const std::uint64_t frames,
    const std::uint32_t sample_rate) noexcept {
    return frames / sample_rate * 1'000'000U +
           frames % sample_rate * 1'000'000U / sample_rate;
}

[[nodiscard]] std::int64_t SignedFramesToMicroseconds(
    const std::int64_t frames,
    const std::uint32_t sample_rate) noexcept {
    const auto magnitude = frames >= 0
                               ? static_cast<std::uint64_t>(frames)
                               : static_cast<std::uint64_t>(-(frames + 1)) + 1U;
    const auto microseconds = FramesToMicroseconds(magnitude, sample_rate);
    if (microseconds > static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max())) {
        return frames >= 0 ? std::numeric_limits<std::int64_t>::max()
                           : std::numeric_limits<std::int64_t>::min();
    }
    const auto signed_value = static_cast<std::int64_t>(microseconds);
    return frames >= 0 ? signed_value : -signed_value;
}

[[nodiscard]] GroupSyncPercentiles Percentiles(std::vector<std::uint64_t> values) {
    GroupSyncPercentiles result;
    if (values.empty()) {
        return result;
    }
    std::ranges::sort(values);
    const auto nearest_rank = [&](const std::uint32_t numerator) {
        const auto count = static_cast<std::uint64_t>(values.size());
        const auto rank = (count * numerator + 99U) / 100U;
        return values[static_cast<std::size_t>(std::max<std::uint64_t>(1U, rank) - 1U)];
    };
    result.p50_microseconds = nearest_rank(50U);
    result.p95_microseconds = nearest_rank(95U);
    result.p99_microseconds = nearest_rank(99U);
    result.maximum_microseconds = values.back();
    return result;
}

[[nodiscard]] std::vector<std::uint64_t> DetectOnsets(
    const std::span<const float> capture,
    const std::uint16_t channel_count,
    const std::uint16_t channel,
    const float threshold,
    const std::uint64_t minimum_gap_frames) {
    std::vector<std::uint64_t> onsets;
    const auto frame_count = capture.size() / channel_count;
    bool armed = true;
    std::uint64_t last_onset = 0U;
    for (std::uint64_t frame = 0U; frame < frame_count; ++frame) {
        const auto sample = capture[static_cast<std::size_t>(frame) * channel_count + channel];
        const auto magnitude = std::isfinite(sample) ? std::abs(sample) : 0.0F;
        if (magnitude < threshold * 0.5F) {
            armed = true;
        }
        const bool gap_ready = onsets.empty() || frame - last_onset >= minimum_gap_frames;
        if (armed && gap_ready && magnitude >= threshold) {
            onsets.push_back(frame);
            last_onset = frame;
            armed = false;
        }
    }
    return onsets;
}

[[nodiscard]] std::optional<std::uint64_t> MatchNearestOnset(
    const std::span<const std::uint64_t> onsets,
    std::size_t& cursor,
    const std::uint64_t reference,
    const std::uint64_t maximum_skew_frames) noexcept {
    const auto lower = reference > maximum_skew_frames
                           ? reference - maximum_skew_frames
                           : 0U;
    const auto upper = reference > std::numeric_limits<std::uint64_t>::max() -
                                      maximum_skew_frames
                           ? std::numeric_limits<std::uint64_t>::max()
                           : reference + maximum_skew_frames;
    while (cursor < onsets.size() && onsets[cursor] < lower) {
        ++cursor;
    }
    if (cursor >= onsets.size() || onsets[cursor] > upper) {
        return std::nullopt;
    }
    auto best = cursor;
    if (cursor + 1U < onsets.size() && onsets[cursor + 1U] <= upper) {
        const auto first_distance = onsets[cursor] >= reference
                                        ? onsets[cursor] - reference
                                        : reference - onsets[cursor];
        const auto second_distance = onsets[cursor + 1U] >= reference
                                         ? onsets[cursor + 1U] - reference
                                         : reference - onsets[cursor + 1U];
        if (second_distance < first_distance) {
            best = cursor + 1U;
        }
    }
    cursor = best + 1U;
    return onsets[best];
}

[[nodiscard]] RefinedLag RefineLag(
    const std::span<const float> capture,
    const GroupSyncWaveformConfig& config,
    const std::uint16_t reference_channel,
    const std::uint16_t member_channel,
    const std::uint64_t reference_onset,
    const std::int64_t onset_lag) noexcept {
    const auto frame_count = static_cast<std::uint64_t>(
        capture.size() / config.channel_count);
    const auto pre_roll = static_cast<std::int64_t>(
        std::min(config.correlation_window_frames / 4U,
                 config.correlation_window_frames - 1U));
    const auto reference_start = static_cast<std::int64_t>(reference_onset) - pre_roll;
    if (reference_start < 0) {
        return {.frames = onset_lag};
    }

    RefinedLag best{.frames = onset_lag};
    double best_score = -1.0;
    const auto search = static_cast<std::int64_t>(config.correlation_search_frames);
    for (auto delta = -search; delta <= search; ++delta) {
        const auto candidate_lag = onset_lag + delta;
        const auto member_start = reference_start + candidate_lag;
        if (member_start < 0 ||
            static_cast<std::uint64_t>(reference_start) +
                    config.correlation_window_frames >
                frame_count ||
            static_cast<std::uint64_t>(member_start) +
                    config.correlation_window_frames >
                frame_count) {
            continue;
        }
        long double dot = 0.0L;
        long double reference_energy = 0.0L;
        long double member_energy = 0.0L;
        for (std::uint32_t index = 0U; index < config.correlation_window_frames;
             ++index) {
            const auto reference_sample = capture[
                (static_cast<std::size_t>(reference_start) + index) *
                    config.channel_count +
                reference_channel];
            const auto member_sample = capture[
                (static_cast<std::size_t>(member_start) + index) *
                    config.channel_count +
                member_channel];
            if (!std::isfinite(reference_sample) || !std::isfinite(member_sample)) {
                continue;
            }
            dot += static_cast<long double>(reference_sample) * member_sample;
            reference_energy +=
                static_cast<long double>(reference_sample) * reference_sample;
            member_energy += static_cast<long double>(member_sample) * member_sample;
        }
        const auto denominator = std::sqrt(reference_energy * member_energy);
        if (denominator <= std::numeric_limits<long double>::epsilon()) {
            continue;
        }
        const auto score = static_cast<double>(dot / denominator);
        const auto current_distance = std::abs(candidate_lag - onset_lag);
        const auto best_distance = std::abs(best.frames - onset_lag);
        if (score > best_score ||
            (score == best_score && current_distance < best_distance)) {
            best_score = score;
            best.frames = candidate_lag;
            best.correlation = score;
        }
    }
    best.refined = best_score >= config.minimum_correlation;
    if (!best.refined) {
        best.frames = onset_lag;
        best.correlation = std::max(0.0, best_score);
    }
    return best;
}

[[nodiscard]] GroupSyncAnalysisResult BuildResult(
    const std::span<const GroupMemberId> member_ids,
    const std::span<const PulseMeasurement> pulses,
    const std::uint64_t reference_pulses,
    const std::uint64_t incomplete_pulses,
    const std::uint32_t minimum_complete_pulses,
    const GroupSyncTargets& targets) {
    GroupSyncAnalysisResult result;
    result.reference_pulses = reference_pulses;
    result.complete_pulses = pulses.size();
    result.incomplete_pulses = incomplete_pulses;
    if (pulses.size() < minimum_complete_pulses) {
        result.status = GroupSyncAnalysisStatus::InsufficientCompletePulses;
        return result;
    }

    std::vector<std::uint64_t> group_skews;
    group_skews.reserve(pulses.size());
    result.members.reserve(member_ids.size());
    for (const auto& pulse : pulses) {
        const auto [minimum, maximum] = std::ranges::minmax(pulse.offsets_microseconds);
        const auto skew = static_cast<long double>(maximum) - minimum;
        group_skews.push_back(skew >= static_cast<long double>(
                                         std::numeric_limits<std::uint64_t>::max())
                                  ? std::numeric_limits<std::uint64_t>::max()
                                  : static_cast<std::uint64_t>(skew));
        for (std::size_t member_index = 1U;
             member_index < pulse.correlation_refined.size(); ++member_index) {
            const auto refined = pulse.correlation_refined[member_index];
            result.correlation_refinements += refined ? 1U : 0U;
            result.onset_fallbacks += refined ? 0U : 1U;
        }
    }
    result.first_alignment_microseconds = group_skews.front();
    result.group_skew = Percentiles(group_skews);

    for (std::size_t member_index = 0U; member_index < member_ids.size(); ++member_index) {
        std::vector<std::uint64_t> absolute_offsets;
        absolute_offsets.reserve(pulses.size());
        std::int64_t minimum = std::numeric_limits<std::int64_t>::max();
        std::int64_t maximum = std::numeric_limits<std::int64_t>::min();
        long double offset_sum = 0.0L;
        long double correlation_sum = 0.0L;
        for (const auto& pulse : pulses) {
            const auto offset = pulse.offsets_microseconds[member_index];
            minimum = std::min(minimum, offset);
            maximum = std::max(maximum, offset);
            offset_sum += offset;
            correlation_sum += pulse.correlations[member_index];
            absolute_offsets.push_back(AbsoluteMicroseconds(offset));
        }
        result.members.push_back({
            .member_id = member_ids[member_index],
            .matched_pulses = pulses.size(),
            .mean_offset_microseconds = static_cast<std::int64_t>(
                std::llround(offset_sum / pulses.size())),
            .minimum_offset_microseconds = minimum,
            .maximum_offset_microseconds = maximum,
            .absolute_offset = Percentiles(std::move(absolute_offsets)),
            .mean_correlation = static_cast<double>(correlation_sum / pulses.size()),
        });
    }
    result.status = GroupSyncAnalysisStatus::Ok;
    result.meets_targets =
        result.group_skew.p95_microseconds <= targets.p95_microseconds &&
        result.group_skew.p99_microseconds <= targets.p99_microseconds &&
        result.first_alignment_microseconds <= targets.first_alignment_microseconds;
    return result;
}

}  // namespace

bool GroupSyncWaveformConfig::IsValid() const noexcept {
    if (sample_rate < 8'000U || sample_rate > 384'000U || channel_count < 2U ||
        channel_count > 8U || members.size() < 2U || members.size() > 4U ||
        !std::isfinite(onset_threshold) || onset_threshold <= 0.0F ||
        onset_threshold > 1.0F || minimum_pulse_gap_milliseconds < 10U ||
        minimum_pulse_gap_milliseconds > 10'000U ||
        maximum_absolute_skew_milliseconds < 1U ||
        maximum_absolute_skew_milliseconds > 1'000U ||
        correlation_window_frames < 8U || correlation_window_frames > 4'096U ||
        correlation_search_frames > 512U || !std::isfinite(minimum_correlation) ||
        minimum_correlation < 0.0 || minimum_correlation > 1.0 ||
        minimum_complete_pulses == 0U || !targets.IsValid()) {
        return false;
    }
    std::vector<GroupMemberId> ids;
    ids.reserve(members.size());
    std::unordered_set<std::uint16_t> channels;
    for (const auto& member : members) {
        if (member.channel >= channel_count || !channels.insert(member.channel).second) {
            return false;
        }
        ids.push_back(member.member_id);
    }
    return HasUniqueMembers(ids);
}

bool GroupSyncTimestampConfig::IsValid() const noexcept {
    return members.size() >= 2U && members.size() <= 4U &&
           minimum_complete_pulses > 0U && targets.IsValid() &&
           HasUniqueMembers(members);
}

GroupSyncAnalysisResult GroupSyncAnalyzer::AnalyzeWaveform(
    const std::span<const float> interleaved_capture,
    const GroupSyncWaveformConfig& config) {
    if (!config.IsValid() || interleaved_capture.empty() ||
        interleaved_capture.size() % config.channel_count != 0U) {
        return {.status = GroupSyncAnalysisStatus::InvalidConfiguration};
    }
    const auto minimum_gap_frames =
        static_cast<std::uint64_t>(config.sample_rate) *
        config.minimum_pulse_gap_milliseconds / 1'000U;
    const auto maximum_skew_frames =
        static_cast<std::uint64_t>(config.sample_rate) *
        config.maximum_absolute_skew_milliseconds / 1'000U;
    std::vector<std::vector<std::uint64_t>> member_onsets;
    member_onsets.reserve(config.members.size());
    for (const auto& member : config.members) {
        member_onsets.push_back(DetectOnsets(interleaved_capture, config.channel_count,
                                             member.channel, config.onset_threshold,
                                             minimum_gap_frames));
    }
    if (member_onsets.front().empty()) {
        return {.status = GroupSyncAnalysisStatus::NoReferencePulses};
    }

    std::vector<GroupMemberId> member_ids;
    member_ids.reserve(config.members.size());
    for (const auto& member : config.members) {
        member_ids.push_back(member.member_id);
    }
    std::vector<std::size_t> cursors(config.members.size(), 0U);
    std::vector<PulseMeasurement> measurements;
    measurements.reserve(member_onsets.front().size());
    std::uint64_t incomplete = 0U;
    for (const auto reference_onset : member_onsets.front()) {
        PulseMeasurement measurement;
        measurement.offsets_microseconds.assign(config.members.size(), 0);
        measurement.correlations.assign(config.members.size(), 1.0);
        measurement.correlation_refined.assign(config.members.size(), true);
        bool complete = true;
        for (std::size_t member_index = 1U; member_index < config.members.size();
             ++member_index) {
            const auto matched = MatchNearestOnset(member_onsets[member_index],
                                                   cursors[member_index],
                                                   reference_onset,
                                                   maximum_skew_frames);
            if (!matched.has_value()) {
                complete = false;
                break;
            }
            const auto onset_lag =
                static_cast<std::int64_t>(*matched) -
                static_cast<std::int64_t>(reference_onset);
            const auto refined = RefineLag(
                interleaved_capture, config, config.members.front().channel,
                config.members[member_index].channel, reference_onset, onset_lag);
            measurement.offsets_microseconds[member_index] =
                SignedFramesToMicroseconds(refined.frames, config.sample_rate);
            measurement.correlations[member_index] = refined.correlation;
            measurement.correlation_refined[member_index] = refined.refined;
        }
        if (complete) {
            measurements.push_back(std::move(measurement));
        } else {
            ++incomplete;
        }
    }
    return BuildResult(member_ids, measurements, member_onsets.front().size(), incomplete,
                       config.minimum_complete_pulses, config.targets);
}

GroupSyncAnalysisResult GroupSyncAnalyzer::AnalyzeTimestamps(
    const std::span<const GroupSyncTimestampObservation> observations,
    const GroupSyncTimestampConfig& config) {
    if (!config.IsValid() || observations.empty()) {
        return {.status = GroupSyncAnalysisStatus::InvalidConfiguration};
    }
    std::unordered_map<GroupMemberId, std::size_t> member_indexes;
    member_indexes.reserve(config.members.size());
    for (std::size_t index = 0U; index < config.members.size(); ++index) {
        member_indexes.emplace(config.members[index], index);
    }
    std::map<std::uint64_t, std::vector<std::optional<std::int64_t>>> pulses;
    for (const auto& observation : observations) {
        const auto found = member_indexes.find(observation.member_id);
        if (found == member_indexes.end()) {
            continue;
        }
        auto [pulse, inserted] = pulses.try_emplace(
            observation.pulse_index,
            std::vector<std::optional<std::int64_t>>(config.members.size()));
        static_cast<void>(inserted);
        if (pulse->second[found->second].has_value()) {
            return {.status = GroupSyncAnalysisStatus::InvalidConfiguration};
        }
        pulse->second[found->second] = observation.presentation_time_nanoseconds;
    }

    std::vector<PulseMeasurement> measurements;
    measurements.reserve(pulses.size());
    std::uint64_t reference_pulses = 0U;
    std::uint64_t incomplete = 0U;
    for (const auto& [pulse_index, timestamps] : pulses) {
        static_cast<void>(pulse_index);
        if (timestamps.front().has_value()) {
            ++reference_pulses;
        }
        if (std::ranges::any_of(timestamps,
                                [](const auto& value) { return !value.has_value(); })) {
            ++incomplete;
            continue;
        }
        PulseMeasurement measurement;
        measurement.offsets_microseconds.reserve(timestamps.size());
        measurement.correlations.assign(timestamps.size(), 1.0);
        const auto reference = *timestamps.front();
        for (const auto& timestamp : timestamps) {
            measurement.offsets_microseconds.push_back(NanosecondsToMicroseconds(
                static_cast<long double>(*timestamp) - reference));
        }
        measurements.push_back(std::move(measurement));
    }
    if (reference_pulses == 0U) {
        return {.status = GroupSyncAnalysisStatus::NoReferencePulses};
    }
    return BuildResult(config.members, measurements, reference_pulses, incomplete,
                       config.minimum_complete_pulses, config.targets);
}

}  // namespace airplaywin::group
