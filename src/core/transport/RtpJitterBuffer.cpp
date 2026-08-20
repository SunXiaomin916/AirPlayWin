#include "core/transport/RtpJitterBuffer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace airplaywin::transport {

RtpJitterBuffer::RtpJitterBuffer(const RtpJitterBufferConfig config)
    : config_(config), slots_(config.capacity_packets),
      current_target_packets_(config.target_packets) {
    if (!config.IsValid()) {
        throw std::invalid_argument("invalid RTP jitter buffer configuration");
    }
}

bool RtpJitterBuffer::Insert(const RtpPacketView& packet,
                             const std::int64_t arrival_time_nanoseconds) noexcept {
    std::scoped_lock lock{mutex_};
    ++received_packets_;
    const auto previous_highest = highest_sequence_;
    const auto extended_sequence = ExtendSequence(packet.sequence_number);
    if (sequence_initialized_ && extended_sequence < next_sequence_) {
        ++late_packets_;
        RecordDisturbanceLocked(1U);
        return false;
    }
    if (extended_sequence - next_sequence_ >= config_.capacity_packets) {
        const auto new_next = extended_sequence - config_.capacity_packets + 1U;
        lost_packets_ += new_next - next_sequence_;
        overflow_packets_ += buffered_packets_;
        for (auto& slot : slots_) {
            slot.occupied = false;
        }
        buffered_packets_ = 0U;
        next_sequence_ = new_next;
        warmed_up_ = false;
        RecordDisturbanceLocked(2U);
    }
    auto& slot = slots_[extended_sequence % slots_.size()];
    if (slot.occupied) {
        if (slot.packet.extended_sequence_number == extended_sequence) {
            ++duplicate_packets_;
        } else {
            ++overflow_packets_;
        }
        return false;
    }
    if (extended_sequence < previous_highest) {
        ++reordered_packets_;
    }
    slot.occupied = true;
    slot.packet.extended_sequence_number = extended_sequence;
    slot.packet.sequence_number = packet.sequence_number;
    slot.packet.timestamp = packet.timestamp;
    slot.packet.ssrc = packet.ssrc;
    slot.packet.payload_type = packet.payload_type;
    slot.packet.marker = packet.marker;
    slot.packet.arrival_time_nanoseconds = arrival_time_nanoseconds;
    slot.packet.payload_size = packet.payload.size();
    if (!packet.payload.empty()) {
        std::memcpy(slot.packet.payload.data(), packet.payload.data(), packet.payload.size());
    }
    ++buffered_packets_;
    UpdateJitter(packet.timestamp, arrival_time_nanoseconds);
    if (config_.adaptive_enabled && ++adaptation_observation_count_ >= 16U) {
        adaptation_observation_count_ = 0U;
        UpdateAdaptiveTargetLocked();
    }
    return true;
}

JitterPopKind RtpJitterBuffer::Pop(BufferedRtpPacket& packet) noexcept {
    std::scoped_lock lock{mutex_};
    if (!sequence_initialized_) {
        return JitterPopKind::Waiting;
    }
    if (!warmed_up_) {
        if (buffered_packets_ < current_target_packets_) {
            return JitterPopKind::Waiting;
        }
        warmed_up_ = true;
        if (config_.adaptive_enabled) {
            adaptive_state_ = AdaptiveJitterState::Locked;
            consecutive_stable_packets_ = 0U;
        }
    }
    auto& slot = slots_[next_sequence_ % slots_.size()];
    if (slot.occupied && slot.packet.extended_sequence_number == next_sequence_) {
        packet = slot.packet;
        slot.occupied = false;
        --buffered_packets_;
        ++next_sequence_;
        ++emitted_packets_;
        RecordStablePacketLocked();
        return JitterPopKind::Packet;
    }
    // Keep the configured reorder depth after warm-up as well. Audio and retransmission
    // packets arrive on separate UDP sockets, so a later sequence can legitimately win the
    // scheduling race. Declaring loss with only one packet ahead makes that retransmission
    // late before it has a chance to enter the buffer.
    if (buffered_packets_ >= current_target_packets_ && highest_sequence_ > next_sequence_) {
        packet = {};
        packet.extended_sequence_number = next_sequence_;
        packet.sequence_number = static_cast<std::uint16_t>(next_sequence_ & 0xFFFFU);
        ++next_sequence_;
        ++lost_packets_;
        RecordDisturbanceLocked(1U);
        return JitterPopKind::Missing;
    }
    return JitterPopKind::Waiting;
}

void RtpJitterBuffer::ReportDownstreamUnderrun(const std::uint64_t count) noexcept {
    if (count == 0U) {
        return;
    }
    std::scoped_lock lock{mutex_};
    downstream_underruns_ += count;
    const auto maximum_growth = config_.EffectiveMaximumTargetPackets();
    const auto growth = static_cast<std::size_t>(
        std::min<std::uint64_t>(count, maximum_growth));
    RecordDisturbanceLocked(std::max<std::size_t>(growth, 1U));
}

void RtpJitterBuffer::Flush() noexcept {
    std::scoped_lock lock{mutex_};
    ResetTimelineLocked();
    ++flush_count_;
}

RtpJitterBufferDiagnostics RtpJitterBuffer::Diagnostics() const noexcept {
    std::scoped_lock lock{mutex_};
    const auto jitter_microseconds = config_.clock_rate == 0U
                                         ? 0U
                                         : static_cast<std::uint64_t>(
                                               std::max(0.0, jitter_) * 1'000'000.0 /
                                               static_cast<double>(config_.clock_rate));
    return RtpJitterBufferDiagnostics{
        .buffered_packets = buffered_packets_,
        .target_packets = current_target_packets_,
        .received_packets = received_packets_,
        .emitted_packets = emitted_packets_,
        .lost_packets = lost_packets_,
        .late_packets = late_packets_,
        .duplicate_packets = duplicate_packets_,
        .reordered_packets = reordered_packets_,
        .overflow_packets = overflow_packets_,
        .flush_count = flush_count_,
        .highest_extended_sequence = highest_sequence_,
        .interarrival_jitter_timestamp_units = jitter_,
        .interarrival_jitter_microseconds = jitter_microseconds,
        .adaptive_enabled = config_.adaptive_enabled,
        .adaptive_state = adaptive_state_,
        .minimum_target_packets = config_.adaptive_enabled
                                      ? config_.minimum_target_packets
                                      : config_.target_packets,
        .maximum_target_packets = config_.adaptive_enabled
                                      ? config_.EffectiveMaximumTargetPackets()
                                      : config_.target_packets,
        .jitter_p95_microseconds =
            static_cast<std::uint64_t>(PercentileJitterTicksLocked(95U)) * 1'000'000U /
            config_.clock_rate,
        .jitter_p99_microseconds =
            static_cast<std::uint64_t>(PercentileJitterTicksLocked(99U)) * 1'000'000U /
            config_.clock_rate,
        .observed_packet_duration_microseconds =
            static_cast<std::uint64_t>(std::max(0.0, observed_packet_duration_ticks_)) *
            1'000'000U / config_.clock_rate,
        .target_increase_events = target_increase_events_,
        .target_decrease_events = target_decrease_events_,
        .degradation_events = degradation_events_,
        .downstream_underruns = downstream_underruns_,
        .consecutive_stable_packets = consecutive_stable_packets_,
    };
}

std::uint64_t RtpJitterBuffer::ExtendSequence(const std::uint16_t sequence_number) noexcept {
    if (!sequence_initialized_) {
        sequence_initialized_ = true;
        next_sequence_ = sequence_number;
        highest_sequence_ = sequence_number;
        return sequence_number;
    }
    const auto highest_low = static_cast<std::uint16_t>(highest_sequence_ & 0xFFFFU);
    const auto unsigned_delta = static_cast<std::uint16_t>(sequence_number - highest_low);
    const auto delta = static_cast<std::int16_t>(unsigned_delta);
    const auto candidate = static_cast<std::int64_t>(highest_sequence_) + delta;
    const auto extended = candidate < 0 ? 0U : static_cast<std::uint64_t>(candidate);
    highest_sequence_ = std::max(highest_sequence_, extended);
    return extended;
}

void RtpJitterBuffer::ResetTimelineLocked() noexcept {
    for (auto& slot : slots_) {
        slot.occupied = false;
    }
    sequence_initialized_ = false;
    warmed_up_ = false;
    transit_initialized_ = false;
    next_sequence_ = 0U;
    highest_sequence_ = 0U;
    jitter_base_timestamp_ = 0U;
    jitter_base_arrival_ns_ = 0;
    previous_transit_ = 0;
    jitter_ = 0.0;
    current_target_packets_ = config_.target_packets;
    adaptive_state_ = AdaptiveJitterState::Warmup;
    jitter_samples_.fill(0U);
    jitter_sample_count_ = 0U;
    jitter_sample_cursor_ = 0U;
    timestamp_observation_initialized_ = false;
    previous_observed_timestamp_ = 0U;
    observed_packet_duration_ticks_ = 0.0;
    adaptation_observation_count_ = 0U;
    consecutive_stable_packets_ = 0U;
    buffered_packets_ = 0U;
}

void RtpJitterBuffer::UpdateJitter(const std::uint32_t timestamp,
                                   const std::int64_t arrival_time_nanoseconds) noexcept {
    if (!transit_initialized_) {
        transit_initialized_ = true;
        jitter_base_timestamp_ = timestamp;
        jitter_base_arrival_ns_ = arrival_time_nanoseconds;
        previous_transit_ = 0;
        timestamp_observation_initialized_ = true;
        previous_observed_timestamp_ = timestamp;
        return;
    }
    const auto arrival_delta_ns = arrival_time_nanoseconds - jitter_base_arrival_ns_;
    const auto arrival_ticks = static_cast<std::int64_t>(
        static_cast<long double>(arrival_delta_ns) * config_.clock_rate / 1'000'000'000.0L);
    const auto timestamp_delta =
        static_cast<std::int64_t>(static_cast<std::int32_t>(timestamp - jitter_base_timestamp_));
    const auto transit = arrival_ticks - timestamp_delta;
    const auto difference = std::abs(transit - previous_transit_);
    previous_transit_ = transit;
    jitter_ += (static_cast<double>(difference) - jitter_) / 16.0;
    jitter_samples_[jitter_sample_cursor_] = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(static_cast<std::uint64_t>(difference),
                                std::numeric_limits<std::uint32_t>::max()));
    jitter_sample_cursor_ = (jitter_sample_cursor_ + 1U) % jitter_samples_.size();
    jitter_sample_count_ = std::min(jitter_sample_count_ + 1U, jitter_samples_.size());

    if (!timestamp_observation_initialized_) {
        timestamp_observation_initialized_ = true;
        previous_observed_timestamp_ = timestamp;
        return;
    }
    const auto observed_timestamp_delta =
        static_cast<std::int32_t>(timestamp - previous_observed_timestamp_);
    previous_observed_timestamp_ = timestamp;
    if (observed_timestamp_delta > 0 &&
        static_cast<std::uint32_t>(observed_timestamp_delta) <= config_.clock_rate) {
        const auto duration = static_cast<double>(observed_timestamp_delta);
        observed_packet_duration_ticks_ = observed_packet_duration_ticks_ == 0.0
                                              ? duration
                                              : observed_packet_duration_ticks_ +
                                                    (duration - observed_packet_duration_ticks_) /
                                                        16.0;
    }
}

void RtpJitterBuffer::RecordDisturbanceLocked(const std::size_t minimum_growth) noexcept {
    if (!config_.adaptive_enabled) {
        return;
    }
    consecutive_stable_packets_ = 0U;
    if (adaptive_state_ != AdaptiveJitterState::Degraded) {
        adaptive_state_ = AdaptiveJitterState::Degraded;
        ++degradation_events_;
    }
    const auto maximum_target = config_.EffectiveMaximumTargetPackets();
    const auto desired = DesiredTargetPacketsLocked();
    const auto grown = current_target_packets_ > maximum_target -
                                                   std::min(minimum_growth, maximum_target)
                           ? maximum_target
                           : current_target_packets_ + minimum_growth;
    const auto next_target = std::min(maximum_target, std::max(grown, desired));
    if (next_target > current_target_packets_) {
        current_target_packets_ = next_target;
        ++target_increase_events_;
    }
}

void RtpJitterBuffer::RecordStablePacketLocked() noexcept {
    if (!config_.adaptive_enabled) {
        return;
    }
    ++consecutive_stable_packets_;
    if (adaptive_state_ == AdaptiveJitterState::Degraded &&
        consecutive_stable_packets_ >= config_.recovery_window_packets) {
        adaptive_state_ = AdaptiveJitterState::Recovery;
        consecutive_stable_packets_ = 0U;
        return;
    }
    if (adaptive_state_ == AdaptiveJitterState::Recovery &&
        consecutive_stable_packets_ >= config_.recovery_window_packets) {
        adaptive_state_ = AdaptiveJitterState::Locked;
        consecutive_stable_packets_ = 0U;
        return;
    }
    if ((adaptive_state_ == AdaptiveJitterState::Locked ||
         adaptive_state_ == AdaptiveJitterState::LowLatency) &&
        consecutive_stable_packets_ >= config_.stable_window_packets) {
        const auto desired = DesiredTargetPacketsLocked();
        if (current_target_packets_ > desired) {
            --current_target_packets_;
            ++target_decrease_events_;
        }
        adaptive_state_ = current_target_packets_ <= desired
                              ? AdaptiveJitterState::LowLatency
                              : AdaptiveJitterState::Locked;
        consecutive_stable_packets_ = 0U;
    }
}

void RtpJitterBuffer::UpdateAdaptiveTargetLocked() noexcept {
    if (!config_.adaptive_enabled) {
        return;
    }
    const auto desired = DesiredTargetPacketsLocked();
    if (desired > current_target_packets_) {
        RecordDisturbanceLocked(desired - current_target_packets_);
    }
}

std::uint32_t RtpJitterBuffer::PercentileJitterTicksLocked(
    const std::uint32_t percentile) const noexcept {
    if (jitter_sample_count_ == 0U) {
        return 0U;
    }
    auto sorted = jitter_samples_;
    std::sort(sorted.begin(), sorted.begin() +
                                  static_cast<std::ptrdiff_t>(jitter_sample_count_));
    const auto bounded_percentile = std::clamp(percentile, 1U, 100U);
    const auto index = ((jitter_sample_count_ - 1U) * bounded_percentile + 99U) / 100U;
    return sorted[index];
}

std::size_t RtpJitterBuffer::DesiredTargetPacketsLocked() const noexcept {
    if (!config_.adaptive_enabled || observed_packet_duration_ticks_ < 1.0) {
        return config_.target_packets;
    }
    const auto p99 = static_cast<double>(PercentileJitterTicksLocked(99U));
    const auto jitter_packets = static_cast<std::size_t>(
        std::ceil(p99 / observed_packet_duration_ticks_));
    return std::clamp(config_.minimum_target_packets + jitter_packets,
                      config_.minimum_target_packets,
                      config_.EffectiveMaximumTargetPackets());
}

}  // namespace airplaywin::transport
