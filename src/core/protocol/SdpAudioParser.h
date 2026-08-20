#pragma once

#include <cstdint>
#include <expected>
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
};

[[nodiscard]] std::expected<audio::EncodedAudioFormat, SdpParseError>
ParseSdpAudioDescription(std::string_view sdp);

}  // namespace airplaywin::protocol
