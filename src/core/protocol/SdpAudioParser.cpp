#include "core/protocol/SdpAudioParser.h"

#include <charconv>
#include <optional>
#include <string>
#include <vector>

#include "core/protocol/RtspTypes.h"

namespace airplaywin::protocol {

namespace {

[[nodiscard]] std::string_view Trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                             value.back() == '\r')) {
        value.remove_suffix(1U);
    }
    return value;
}

template <typename Integer>
[[nodiscard]] bool ParseUnsigned(const std::string_view text, Integer& value) noexcept {
    if (text.empty() || text.front() == '+' || text.front() == '-') {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

[[nodiscard]] std::vector<std::string_view> Split(const std::string_view text,
                                                  const char separator) {
    std::vector<std::string_view> parts;
    std::size_t begin = 0U;
    while (begin <= text.size()) {
        const auto end = text.find(separator, begin);
        parts.push_back(text.substr(begin, end == std::string_view::npos ? end : end - begin));
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    return parts;
}

}  // namespace

std::expected<audio::EncodedAudioFormat, SdpParseError> ParseSdpAudioDescription(
    const std::string_view sdp) {
    if (sdp.empty()) {
        return std::unexpected{SdpParseError::Empty};
    }
    std::optional<std::uint8_t> payload_type;
    std::optional<audio::EncodedAudioFormat> mapped_format;
    std::optional<std::uint32_t> frames_per_packet;
    std::size_t line_count = 0U;
    std::size_t line_start = 0U;
    while (line_start <= sdp.size()) {
        const auto line_end = sdp.find('\n', line_start);
        auto line = Trim(sdp.substr(line_start, line_end == std::string_view::npos
                                                   ? line_end
                                                   : line_end - line_start));
        ++line_count;
        if (line_count > 256U) {
            return std::unexpected{SdpParseError::TooManyLines};
        }
        if (line.size() > 2'048U) {
            return std::unexpected{SdpParseError::LineTooLong};
        }
        if (line.starts_with("m=audio ")) {
            const auto fields = Split(line, ' ');
            if (fields.size() < 4U || fields[2] != "RTP/AVP") {
                return std::unexpected{SdpParseError::InvalidMedia};
            }
            std::uint16_t parsed_payload = 0U;
            if (!ParseUnsigned(fields[3], parsed_payload) || parsed_payload > 127U) {
                return std::unexpected{SdpParseError::InvalidMedia};
            }
            payload_type = static_cast<std::uint8_t>(parsed_payload);
        } else if (line.starts_with("a=rtpmap:")) {
            const auto mapping = line.substr(9U);
            const auto space = mapping.find(' ');
            if (space == std::string_view::npos) {
                return std::unexpected{SdpParseError::MissingPayloadMapping};
            }
            std::uint16_t mapped_payload = 0U;
            if (!ParseUnsigned(mapping.substr(0U, space), mapped_payload) ||
                mapped_payload > 127U) {
                return std::unexpected{SdpParseError::MissingPayloadMapping};
            }
            const auto encoding_fields = Split(mapping.substr(space + 1U), '/');
            if (encoding_fields.size() >= 2U &&
                EqualsAsciiCaseInsensitive(encoding_fields[0], "L16")) {
                std::uint32_t sample_rate = 0U;
                if (!ParseUnsigned(encoding_fields[1], sample_rate) ||
                    sample_rate < 8'000U || sample_rate > 384'000U) {
                    return std::unexpected{SdpParseError::InvalidClockRate};
                }
                std::uint16_t channels = 1U;
                if (encoding_fields.size() >= 3U &&
                    (!ParseUnsigned(encoding_fields[2], channels) || channels == 0U ||
                     channels > 8U)) {
                    return std::unexpected{SdpParseError::InvalidChannelCount};
                }
                mapped_format = audio::EncodedAudioFormat{
                    .codec = audio::AudioCodec::PcmL16BigEndian,
                    .sample_rate = sample_rate,
                    .channel_count = channels,
                    .payload_type = static_cast<std::uint8_t>(mapped_payload),
                };
            }
        } else if (line.starts_with("a=framesize:")) {
            const auto value = line.substr(12U);
            const auto space = value.find(' ');
            if (space != std::string_view::npos) {
                std::uint32_t parsed_frames = 0U;
                if (!ParseUnsigned(Trim(value.substr(space + 1U)), parsed_frames) ||
                    parsed_frames == 0U || parsed_frames > 8'192U) {
                    return std::unexpected{SdpParseError::InvalidFramesPerPacket};
                }
                frames_per_packet = parsed_frames;
            }
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 1U;
    }
    if (!payload_type.has_value()) {
        return std::unexpected{SdpParseError::InvalidMedia};
    }
    audio::EncodedAudioFormat format;
    if (*payload_type == 10U) {
        format = {.sample_rate = 44'100U, .channel_count = 2U, .payload_type = 10U};
    } else if (*payload_type == 11U) {
        format = {.sample_rate = 44'100U, .channel_count = 1U, .payload_type = 11U};
    } else if (mapped_format.has_value() && mapped_format->payload_type == *payload_type) {
        format = *mapped_format;
    } else if (mapped_format.has_value()) {
        return std::unexpected{SdpParseError::MissingPayloadMapping};
    } else {
        return std::unexpected{SdpParseError::UnsupportedCodec};
    }
    if (frames_per_packet.has_value()) {
        format.nominal_frames_per_packet = *frames_per_packet;
    }
    return format;
}

}  // namespace airplaywin::protocol
