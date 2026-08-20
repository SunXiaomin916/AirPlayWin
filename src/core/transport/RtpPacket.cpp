#include "core/transport/RtpPacket.h"

#include <limits>

namespace airplaywin::transport {

namespace {

[[nodiscard]] std::uint8_t Byte(const std::span<const std::byte> bytes,
                                const std::size_t index) noexcept {
    return std::to_integer<std::uint8_t>(bytes[index]);
}

[[nodiscard]] std::uint16_t ReadU16(const std::span<const std::byte> bytes,
                                    const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(Byte(bytes, offset)) << 8U) |
                                      Byte(bytes, offset + 1U));
}

[[nodiscard]] std::uint32_t ReadU32(const std::span<const std::byte> bytes,
                                    const std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(Byte(bytes, offset)) << 24U) |
           (static_cast<std::uint32_t>(Byte(bytes, offset + 1U)) << 16U) |
           (static_cast<std::uint32_t>(Byte(bytes, offset + 2U)) << 8U) |
           static_cast<std::uint32_t>(Byte(bytes, offset + 3U));
}

}  // namespace

std::expected<RtpPacketView, RtpParseError> ParseRtpPacket(
    const std::span<const std::byte> datagram) noexcept {
    constexpr std::size_t kFixedHeaderBytes = 12U;
    if (datagram.size() < kFixedHeaderBytes) {
        return std::unexpected{RtpParseError::PacketTooShort};
    }
    const auto first = Byte(datagram, 0U);
    if ((first >> 6U) != 2U) {
        return std::unexpected{RtpParseError::UnsupportedVersion};
    }
    const bool has_padding = (first & 0x20U) != 0U;
    const bool has_extension = (first & 0x10U) != 0U;
    const auto csrc_count = static_cast<std::uint8_t>(first & 0x0FU);
    const auto csrc_bytes = static_cast<std::size_t>(csrc_count) * 4U;
    if (csrc_bytes > datagram.size() - kFixedHeaderBytes) {
        return std::unexpected{RtpParseError::TruncatedCsrcList};
    }
    std::size_t payload_offset = kFixedHeaderBytes + csrc_bytes;
    std::uint16_t extension_profile = 0U;
    std::span<const std::byte> extension;
    if (has_extension) {
        if (datagram.size() - payload_offset < 4U) {
            return std::unexpected{RtpParseError::TruncatedExtension};
        }
        extension_profile = ReadU16(datagram, payload_offset);
        const auto extension_words = ReadU16(datagram, payload_offset + 2U);
        const auto extension_bytes = static_cast<std::size_t>(extension_words) * 4U;
        payload_offset += 4U;
        if (extension_bytes > datagram.size() - payload_offset) {
            return std::unexpected{RtpParseError::TruncatedExtension};
        }
        extension = datagram.subspan(payload_offset, extension_bytes);
        payload_offset += extension_bytes;
    }
    std::size_t payload_end = datagram.size();
    if (has_padding) {
        const auto padding_bytes = static_cast<std::size_t>(Byte(datagram, datagram.size() - 1U));
        if (padding_bytes == 0U || padding_bytes > payload_end - payload_offset) {
            return std::unexpected{RtpParseError::InvalidPadding};
        }
        payload_end -= padding_bytes;
    }
    const auto payload_bytes = payload_end - payload_offset;
    if (payload_bytes > kMaxRtpPayloadBytes) {
        return std::unexpected{RtpParseError::PayloadTooLarge};
    }
    const auto second = Byte(datagram, 1U);
    return RtpPacketView{
        .marker = (second & 0x80U) != 0U,
        .payload_type = static_cast<std::uint8_t>(second & 0x7FU),
        .sequence_number = ReadU16(datagram, 2U),
        .timestamp = ReadU32(datagram, 4U),
        .ssrc = ReadU32(datagram, 8U),
        .csrc_count = csrc_count,
        .extension_profile = extension_profile,
        .extension = extension,
        .payload = datagram.subspan(payload_offset, payload_bytes),
    };
}

}  // namespace airplaywin::transport
