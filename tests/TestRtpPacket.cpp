#include "TestFramework.h"

#include <array>
#include <cstddef>
#include <vector>

#include "RtpTestUtils.h"
#include "core/transport/RtpPacket.h"

void TestRtpPacket() {
    using airplaywin::tests::BuildRtpPacket;
    using airplaywin::transport::ParseRtpPacket;
    using airplaywin::transport::RtpParseError;

    const std::array payload{std::byte{0x12U}, std::byte{0x34U}, std::byte{0x56U}};
    const auto bytes = BuildRtpPacket(0x1234U, 0x0102'0304U, payload, 96U,
                                      0xAABB'CCDDU, true);
    const auto parsed = ParseRtpPacket(bytes);
    APW_EXPECT(parsed.has_value());
    APW_EXPECT(parsed->marker);
    APW_EXPECT(parsed->payload_type == 96U);
    APW_EXPECT(parsed->sequence_number == 0x1234U);
    APW_EXPECT(parsed->timestamp == 0x0102'0304U);
    APW_EXPECT(parsed->ssrc == 0xAABB'CCDDU);
    APW_EXPECT(parsed->payload.size() == payload.size());
    APW_EXPECT(parsed->payload[1] == std::byte{0x34U});

    std::vector<std::byte> extended{
        std::byte{0xB1U}, std::byte{0x60U}, std::byte{0}, std::byte{1},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{2},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{3},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{4},
        std::byte{0xBEU}, std::byte{0xDEU}, std::byte{0}, std::byte{1},
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
        std::byte{0x7FU}, std::byte{0}, std::byte{0}, std::byte{3}};
    const auto parsed_extended = ParseRtpPacket(extended);
    APW_EXPECT(parsed_extended.has_value());
    APW_EXPECT(parsed_extended->csrc_count == 1U);
    APW_EXPECT(parsed_extended->extension_profile == 0xBEDEU);
    APW_EXPECT(parsed_extended->extension.size() == 4U);
    APW_EXPECT(parsed_extended->payload.size() == 1U);
    APW_EXPECT(parsed_extended->payload.front() == std::byte{0x7FU});

    std::array<std::byte, 11U> short_packet{};
    const auto short_result = ParseRtpPacket(short_packet);
    APW_EXPECT(!short_result.has_value());
    APW_EXPECT(short_result.error() == RtpParseError::PacketTooShort);

    auto invalid_version = bytes;
    invalid_version[0] = std::byte{0x40U};
    const auto version_result = ParseRtpPacket(invalid_version);
    APW_EXPECT(!version_result.has_value());
    APW_EXPECT(version_result.error() == RtpParseError::UnsupportedVersion);

    auto invalid_padding = bytes;
    invalid_padding[0] = std::byte{0xA0U};
    invalid_padding.back() = std::byte{0x7FU};
    const auto padding_result = ParseRtpPacket(invalid_padding);
    APW_EXPECT(!padding_result.has_value());
    APW_EXPECT(padding_result.error() == RtpParseError::InvalidPadding);

    auto truncated_extension = bytes;
    truncated_extension.resize(12U);
    truncated_extension[0] = std::byte{0x90U};
    const auto extension_result = ParseRtpPacket(truncated_extension);
    APW_EXPECT(!extension_result.has_value());
    APW_EXPECT(extension_result.error() == RtpParseError::TruncatedExtension);

    std::vector<std::byte> oversized_payload(12U +
                                             airplaywin::transport::kMaxRtpPayloadBytes + 1U);
    oversized_payload[0] = std::byte{0x80U};
    const auto oversized_result = ParseRtpPacket(oversized_payload);
    APW_EXPECT(!oversized_result.has_value());
    APW_EXPECT(oversized_result.error() == RtpParseError::PayloadTooLarge);
}
