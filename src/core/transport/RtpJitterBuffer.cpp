#include "core/transport/RtpJitterBuffer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace airplaywin::transport {

RtpJitterBuffer::RtpJitterBuffer(const RtpJitterBufferConfig config)
    : config_(config), slots_(config.capacity_packets) {
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
    return true;
}

JitterPopKind RtpJitterBuffer::Pop(BufferedRtpPacket& packet) noexcept {
    std::scoped_lock lock{mutex_};
    if (!sequence_initialized_) {
        return JitterPopKind::Waiting;
    }
    if (!warmed_up_) {
        if (buffered_packets_ < config_.target_packets) {
            return JitterPopKind::Waiting;
        }
        warmed_up_ = true;
    }
    auto& slot = slots_[next_sequence_ % slots_.size()];
    if (slot.occupied && slot.packet.extended_sequence_number == next_sequence_) {
        packet = slot.packet;
        slot.occupied = false;
        --buffered_packets_;
        ++next_sequence_;
        ++emitted_packets_;
        return JitterPopKind::Packet;
    }
    if (buffered_packets_ != 0U && highest_sequence_ > next_sequence_) {
        packet = {};
        packet.extended_sequence_number = next_sequence_;
        packet.sequence_number = static_cast<std::uint16_t>(next_sequence_ & 0xFFFFU);
        ++next_sequence_;
        ++lost_packets_;
        return JitterPopKind::Missing;
    }
    return JitterPopKind::Waiting;
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
        .target_packets = config_.target_packets,
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
    buffered_packets_ = 0U;
}

void RtpJitterBuffer::UpdateJitter(const std::uint32_t timestamp,
                                   const std::int64_t arrival_time_nanoseconds) noexcept {
    if (!transit_initialized_) {
        transit_initialized_ = true;
        jitter_base_timestamp_ = timestamp;
        jitter_base_arrival_ns_ = arrival_time_nanoseconds;
        previous_transit_ = 0;
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
}

}  // namespace airplaywin::transport
