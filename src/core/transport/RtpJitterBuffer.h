#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "core/transport/RtpPacket.h"

namespace airplaywin::transport {

struct RtpJitterBufferConfig final {
    std::size_t capacity_packets{256U};
    std::size_t target_packets{4U};
    std::uint32_t clock_rate{44'100U};
    bool adaptive_enabled{false};
    std::size_t minimum_target_packets{2U};
    std::size_t maximum_target_packets{0U};
    std::uint32_t stable_window_packets{128U};
    std::uint32_t recovery_window_packets{32U};

    [[nodiscard]] constexpr std::size_t EffectiveMaximumTargetPackets() const noexcept {
        return maximum_target_packets == 0U ? capacity_packets - 1U
                                             : maximum_target_packets;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        const auto maximum_target = EffectiveMaximumTargetPackets();
        return capacity_packets >= 4U && capacity_packets <= 4'096U &&
               target_packets >= 1U && target_packets < capacity_packets &&
               clock_rate >= 8'000U && clock_rate <= 384'000U &&
               (!adaptive_enabled ||
                (minimum_target_packets >= 1U &&
                 minimum_target_packets <= target_packets &&
                 target_packets <= maximum_target &&
                 maximum_target < capacity_packets && stable_window_packets >= 8U &&
                 recovery_window_packets >= 4U));
    }
};

enum class AdaptiveJitterState : std::uint8_t {
    Warmup,
    Locked,
    LowLatency,
    Degraded,
    Recovery,
};

enum class JitterPopKind : std::uint8_t {
    Waiting,
    Packet,
    Missing,
};

struct RtpJitterBufferDiagnostics final {
    std::size_t buffered_packets{0U};
    std::size_t target_packets{0U};
    std::uint64_t received_packets{0U};
    std::uint64_t emitted_packets{0U};
    std::uint64_t lost_packets{0U};
    std::uint64_t late_packets{0U};
    std::uint64_t duplicate_packets{0U};
    std::uint64_t reordered_packets{0U};
    std::uint64_t overflow_packets{0U};
    std::uint64_t flush_count{0U};
    std::uint64_t highest_extended_sequence{0U};
    double interarrival_jitter_timestamp_units{0.0};
    std::uint64_t interarrival_jitter_microseconds{0U};
    bool adaptive_enabled{false};
    AdaptiveJitterState adaptive_state{AdaptiveJitterState::Warmup};
    std::size_t minimum_target_packets{0U};
    std::size_t maximum_target_packets{0U};
    std::uint64_t jitter_p95_microseconds{0U};
    std::uint64_t jitter_p99_microseconds{0U};
    std::uint64_t observed_packet_duration_microseconds{0U};
    std::uint64_t target_increase_events{0U};
    std::uint64_t target_decrease_events{0U};
    std::uint64_t degradation_events{0U};
    std::uint64_t downstream_underruns{0U};
    std::uint32_t consecutive_stable_packets{0U};
};

class RtpJitterBuffer final {
public:
    explicit RtpJitterBuffer(RtpJitterBufferConfig config = {});

    RtpJitterBuffer(const RtpJitterBuffer&) = delete;
    RtpJitterBuffer& operator=(const RtpJitterBuffer&) = delete;

    [[nodiscard]] bool Insert(const RtpPacketView& packet,
                              std::int64_t arrival_time_nanoseconds) noexcept;
    [[nodiscard]] JitterPopKind Pop(BufferedRtpPacket& packet) noexcept;
    void ReportDownstreamUnderrun(std::uint64_t count = 1U) noexcept;
    void Flush() noexcept;
    [[nodiscard]] RtpJitterBufferDiagnostics Diagnostics() const noexcept;

private:
    struct Slot final {
        bool occupied{false};
        BufferedRtpPacket packet{};
    };

    [[nodiscard]] std::uint64_t ExtendSequence(std::uint16_t sequence_number) noexcept;
    void ResetTimelineLocked() noexcept;
    void UpdateJitter(std::uint32_t timestamp,
                      std::int64_t arrival_time_nanoseconds) noexcept;
    void RecordDisturbanceLocked(std::size_t minimum_growth) noexcept;
    void RecordStablePacketLocked() noexcept;
    void UpdateAdaptiveTargetLocked() noexcept;
    [[nodiscard]] std::uint32_t PercentileJitterTicksLocked(
        std::uint32_t percentile) const noexcept;
    [[nodiscard]] std::size_t DesiredTargetPacketsLocked() const noexcept;

    RtpJitterBufferConfig config_{};
    mutable std::mutex mutex_{};
    std::vector<Slot> slots_{};
    bool sequence_initialized_{false};
    bool warmed_up_{false};
    bool transit_initialized_{false};
    std::uint64_t next_sequence_{0U};
    std::uint64_t highest_sequence_{0U};
    std::uint32_t jitter_base_timestamp_{0U};
    std::int64_t jitter_base_arrival_ns_{0};
    std::int64_t previous_transit_{0};
    double jitter_{0.0};
    std::size_t current_target_packets_{0U};
    AdaptiveJitterState adaptive_state_{AdaptiveJitterState::Warmup};
    std::array<std::uint32_t, 128U> jitter_samples_{};
    std::size_t jitter_sample_count_{0U};
    std::size_t jitter_sample_cursor_{0U};
    bool timestamp_observation_initialized_{false};
    std::uint32_t previous_observed_timestamp_{0U};
    double observed_packet_duration_ticks_{0.0};
    std::uint32_t adaptation_observation_count_{0U};
    std::uint32_t consecutive_stable_packets_{0U};
    std::size_t buffered_packets_{0U};
    std::uint64_t received_packets_{0U};
    std::uint64_t emitted_packets_{0U};
    std::uint64_t lost_packets_{0U};
    std::uint64_t late_packets_{0U};
    std::uint64_t duplicate_packets_{0U};
    std::uint64_t reordered_packets_{0U};
    std::uint64_t overflow_packets_{0U};
    std::uint64_t flush_count_{0U};
    std::uint64_t target_increase_events_{0U};
    std::uint64_t target_decrease_events_{0U};
    std::uint64_t degradation_events_{0U};
    std::uint64_t downstream_underruns_{0U};
};

}  // namespace airplaywin::transport
