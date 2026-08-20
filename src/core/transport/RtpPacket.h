#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace airplaywin::transport {

inline constexpr std::size_t kMaxRtpPayloadBytes = 4U * 1'024U;

enum class RtpParseError : std::uint8_t {
    PacketTooShort,
    UnsupportedVersion,
    TruncatedCsrcList,
    TruncatedExtension,
    InvalidPadding,
    PayloadTooLarge,
};

struct RtpPacketView final {
    bool marker{false};
    std::uint8_t payload_type{0U};
    std::uint16_t sequence_number{0U};
    std::uint32_t timestamp{0U};
    std::uint32_t ssrc{0U};
    std::uint8_t csrc_count{0U};
    std::uint16_t extension_profile{0U};
    std::span<const std::byte> extension{};
    std::span<const std::byte> payload{};
};

struct BufferedRtpPacket final {
    std::uint64_t extended_sequence_number{0U};
    std::uint16_t sequence_number{0U};
    std::uint32_t timestamp{0U};
    std::uint32_t ssrc{0U};
    std::uint8_t payload_type{0U};
    bool marker{false};
    std::int64_t arrival_time_nanoseconds{0};
    std::size_t payload_size{0U};
    std::array<std::byte, kMaxRtpPayloadBytes> payload{};

    [[nodiscard]] std::span<const std::byte> Payload() const noexcept {
        return {payload.data(), payload_size};
    }
};

[[nodiscard]] std::expected<RtpPacketView, RtpParseError> ParseRtpPacket(
    std::span<const std::byte> datagram) noexcept;

}  // namespace airplaywin::transport
