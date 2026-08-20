#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace airplaywin::tests {

[[nodiscard]] inline std::vector<std::byte> BuildRtpPacket(
    const std::uint16_t sequence_number,
    const std::uint32_t timestamp,
    const std::span<const std::byte> payload,
    const std::uint8_t payload_type = 96U,
    const std::uint32_t ssrc = 0x1020'3040U,
    const bool marker = false) {
    std::vector<std::byte> packet(12U + payload.size());
    packet[0] = std::byte{0x80U};
    packet[1] = std::byte{static_cast<std::uint8_t>((marker ? 0x80U : 0U) | payload_type)};
    packet[2] = std::byte{static_cast<std::uint8_t>(sequence_number >> 8U)};
    packet[3] = std::byte{static_cast<std::uint8_t>(sequence_number)};
    packet[4] = std::byte{static_cast<std::uint8_t>(timestamp >> 24U)};
    packet[5] = std::byte{static_cast<std::uint8_t>(timestamp >> 16U)};
    packet[6] = std::byte{static_cast<std::uint8_t>(timestamp >> 8U)};
    packet[7] = std::byte{static_cast<std::uint8_t>(timestamp)};
    packet[8] = std::byte{static_cast<std::uint8_t>(ssrc >> 24U)};
    packet[9] = std::byte{static_cast<std::uint8_t>(ssrc >> 16U)};
    packet[10] = std::byte{static_cast<std::uint8_t>(ssrc >> 8U)};
    packet[11] = std::byte{static_cast<std::uint8_t>(ssrc)};
    for (std::size_t index = 0U; index < payload.size(); ++index) {
        packet[12U + index] = payload[index];
    }
    return packet;
}

}  // namespace airplaywin::tests
