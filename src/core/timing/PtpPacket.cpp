#include "core/timing/PtpPacket.h"

#include <bit>
#include <limits>

namespace airplaywin::timing {
namespace {

[[nodiscard]] std::uint16_t ReadU16(const std::span<const std::byte> bytes,
                                    const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])) << 8U |
           std::to_integer<std::uint8_t>(bytes[offset + 1U]);
}

[[nodiscard]] std::uint32_t ReadU32(const std::span<const std::byte> bytes,
                                    const std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset])) << 24U |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U])) << 16U |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2U])) << 8U |
           std::to_integer<std::uint8_t>(bytes[offset + 3U]);
}

[[nodiscard]] std::uint64_t ReadU64(const std::span<const std::byte> bytes,
                                    const std::size_t offset) noexcept {
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < 8U; ++index) {
        value = value << 8U | std::to_integer<std::uint8_t>(bytes[offset + index]);
    }
    return value;
}

[[nodiscard]] std::optional<std::uint64_t> ReadTimestamp(
    const std::span<const std::byte> bytes, const std::size_t offset) noexcept {
    if (offset + 10U > bytes.size()) {
        return std::nullopt;
    }
    std::uint64_t seconds = 0U;
    for (std::size_t index = 0U; index < 6U; ++index) {
        seconds = seconds << 8U | std::to_integer<std::uint8_t>(bytes[offset + index]);
    }
    const auto nanoseconds = ReadU32(bytes, offset + 6U);
    if (nanoseconds >= 1'000'000'000U ||
        seconds > (std::numeric_limits<std::uint64_t>::max() - nanoseconds) /
                      1'000'000'000U) {
        return std::nullopt;
    }
    return seconds * 1'000'000'000U + nanoseconds;
}

[[nodiscard]] PtpMessageType ToMessageType(const std::uint8_t value) noexcept {
    switch (value) {
    case 0x0U:
        return PtpMessageType::Sync;
    case 0x1U:
        return PtpMessageType::DelayRequest;
    case 0x8U:
        return PtpMessageType::FollowUp;
    case 0x9U:
        return PtpMessageType::DelayResponse;
    case 0xBU:
        return PtpMessageType::Announce;
    default:
        return PtpMessageType::Unknown;
    }
}

}  // namespace

std::optional<PtpPacketView> ParsePtpPacket(
    const std::span<const std::byte> datagram) noexcept {
    constexpr std::size_t kHeaderBytes = 34U;
    if (datagram.size() < kHeaderBytes ||
        (std::to_integer<std::uint8_t>(datagram[1U]) & 0x0FU) != 2U) {
        return std::nullopt;
    }
    const auto message_length = ReadU16(datagram, 2U);
    if (message_length < kHeaderBytes || message_length > datagram.size()) {
        return std::nullopt;
    }
    const auto packet = datagram.first(message_length);
    const auto message_type =
        ToMessageType(std::to_integer<std::uint8_t>(packet[0U]) & 0x0FU);
    const auto flags = ReadU16(packet, 6U);
    const auto correction_scaled = std::bit_cast<std::int64_t>(ReadU64(packet, 8U));
    PtpPacketView result{
        .message_type = message_type,
        .domain_number = std::to_integer<std::uint8_t>(packet[4U]),
        .two_step = (flags & 0x0200U) != 0U,
        .correction_nanoseconds = correction_scaled / 65'536LL,
        .source_clock_identity = ReadU64(packet, 20U),
        .source_port_number = ReadU16(packet, 28U),
        .sequence_id = ReadU16(packet, 30U),
    };
    if (message_type == PtpMessageType::Sync ||
        message_type == PtpMessageType::FollowUp ||
        message_type == PtpMessageType::DelayResponse) {
        result.origin_timestamp_nanoseconds = ReadTimestamp(packet, kHeaderBytes);
        if (!result.origin_timestamp_nanoseconds.has_value()) {
            return std::nullopt;
        }
    }
    return result;
}

}  // namespace airplaywin::timing
