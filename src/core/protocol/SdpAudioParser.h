#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "core/audio/AudioStreamTypes.h"

namespace airplaywin::protocol {

enum class SdpParseError : std::uint8_t {
    Empty,
    TooManyLines,
    LineTooLong,
    InvalidMedia,
    MissingPayloadMapping,
    UnsupportedCodec,
    InvalidClockRate,
    InvalidChannelCount,
    InvalidFramesPerPacket,
    InvalidCodecConfiguration,
};

struct SdpAudioDescription final {
    audio::EncodedAudioFormat format{};
    std::optional<std::string> encrypted_aes_key{};
    std::optional<std::string> aes_initialization_vector{};
};

[[nodiscard]] std::expected<SdpAudioDescription, SdpParseError>
ParseSdpAudioSession(std::string_view sdp);

[[nodiscard]] std::expected<audio::EncodedAudioFormat, SdpParseError>
ParseSdpAudioDescription(std::string_view sdp);

}  // namespace airplaywin::protocol
