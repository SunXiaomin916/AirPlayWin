#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace airplaywin::timing {

enum class PtpMessageType : std::uint8_t {
    Sync = 0x0U,
    DelayRequest = 0x1U,
    FollowUp = 0x8U,
    DelayResponse = 0x9U,
    Announce = 0xBU,
    Unknown = 0xFFU,
};

struct PtpPacketView final {
    PtpMessageType message_type{PtpMessageType::Unknown};
    std::uint8_t domain_number{0U};
    bool two_step{false};
    std::int64_t correction_nanoseconds{0};
    std::uint64_t source_clock_identity{0U};
    std::uint16_t source_port_number{0U};
    std::uint16_t sequence_id{0U};
    std::optional<std::uint64_t> origin_timestamp_nanoseconds{};
};

[[nodiscard]] std::optional<PtpPacketView> ParsePtpPacket(
    std::span<const std::byte> datagram) noexcept;

}  // namespace airplaywin::timing
