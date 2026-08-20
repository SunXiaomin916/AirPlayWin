#pragma once

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

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return capacity_packets >= 4U && capacity_packets <= 4'096U &&
               target_packets >= 1U && target_packets < capacity_packets &&
               clock_rate >= 8'000U && clock_rate <= 384'000U;
    }
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
};

class RtpJitterBuffer final {
public:
    explicit RtpJitterBuffer(RtpJitterBufferConfig config = {});

    RtpJitterBuffer(const RtpJitterBuffer&) = delete;
    RtpJitterBuffer& operator=(const RtpJitterBuffer&) = delete;

    [[nodiscard]] bool Insert(const RtpPacketView& packet,
                              std::int64_t arrival_time_nanoseconds) noexcept;
    [[nodiscard]] JitterPopKind Pop(BufferedRtpPacket& packet) noexcept;
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
    std::size_t buffered_packets_{0U};
    std::uint64_t received_packets_{0U};
    std::uint64_t emitted_packets_{0U};
    std::uint64_t lost_packets_{0U};
    std::uint64_t late_packets_{0U};
    std::uint64_t duplicate_packets_{0U};
    std::uint64_t reordered_packets_{0U};
    std::uint64_t overflow_packets_{0U};
    std::uint64_t flush_count_{0U};
};

}  // namespace airplaywin::transport
