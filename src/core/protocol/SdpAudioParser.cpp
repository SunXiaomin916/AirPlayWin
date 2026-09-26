#include "core/protocol/SdpAudioParser.h"

#include <array>
#include <charconv>
#include <limits>
#include <optional>
#include <string>
#include <utility>
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

[[nodiscard]] std::vector<std::string_view> SplitWhitespace(std::string_view text) {
    std::vector<std::string_view> parts;
    while (!text.empty()) {
        text = Trim(text);
        if (text.empty()) {
            break;
        }
        const auto end = text.find_first_of(" \t");
        parts.push_back(text.substr(0U, end));
        if (end == std::string_view::npos) {
            break;
        }
        text.remove_prefix(end + 1U);
    }
    return parts;
}

void WriteBigEndian16(std::array<std::byte, 24U>& output,
                      const std::size_t offset,
                      const std::uint16_t value) noexcept {
    output[offset] = static_cast<std::byte>(value >> 8U);
    output[offset + 1U] = static_cast<std::byte>(value);
}

void WriteBigEndian32(std::array<std::byte, 24U>& output,
                      const std::size_t offset,
                      const std::uint32_t value) noexcept {
    output[offset] = static_cast<std::byte>(value >> 24U);
    output[offset + 1U] = static_cast<std::byte>(value >> 16U);
    output[offset + 2U] = static_cast<std::byte>(value >> 8U);
    output[offset + 3U] = static_cast<std::byte>(value);
}

struct AlacFmtp final {
    std::uint32_t frame_length{0U};
    std::uint8_t compatible_version{0U};
    std::uint8_t bit_depth{0U};
    std::uint8_t pb{0U};
    std::uint8_t mb{0U};
    std::uint8_t kb{0U};
    std::uint8_t channel_count{0U};
    std::uint16_t max_run{0U};
    std::uint32_t max_frame_bytes{0U};
    std::uint32_t average_bit_rate{0U};
    std::uint32_t sample_rate{0U};
};

[[nodiscard]] std::optional<AlacFmtp> ParseAlacFmtp(const std::string_view text) {
    const auto fields = SplitWhitespace(text);
    if (fields.size() != 11U) {
        return std::nullopt;
    }
    std::array<std::uint64_t, 11U> values{};
    for (std::size_t index = 0U; index < fields.size(); ++index) {
        if (!ParseUnsigned(fields[index], values[index])) {
            return std::nullopt;
        }
    }
    if (values[0] == 0U || values[0] > 8'192U || values[1] > 255U ||
        (values[2] != 16U && values[2] != 24U && values[2] != 32U) ||
        values[3] > 255U || values[4] > 255U || values[5] > 255U ||
        values[6] == 0U || values[6] > 8U || values[7] > 65'535U ||
        values[8] > std::numeric_limits<std::uint32_t>::max() ||
        values[9] > std::numeric_limits<std::uint32_t>::max() ||
        values[10] < 8'000U || values[10] > 384'000U) {
        return std::nullopt;
    }
    return AlacFmtp{
        .frame_length = static_cast<std::uint32_t>(values[0]),
        .compatible_version = static_cast<std::uint8_t>(values[1]),
        .bit_depth = static_cast<std::uint8_t>(values[2]),
        .pb = static_cast<std::uint8_t>(values[3]),
        .mb = static_cast<std::uint8_t>(values[4]),
        .kb = static_cast<std::uint8_t>(values[5]),
        .channel_count = static_cast<std::uint8_t>(values[6]),
        .max_run = static_cast<std::uint16_t>(values[7]),
        .max_frame_bytes = static_cast<std::uint32_t>(values[8]),
        .average_bit_rate = static_cast<std::uint32_t>(values[9]),
        .sample_rate = static_cast<std::uint32_t>(values[10]),
    };
}

[[nodiscard]] std::array<std::byte, 24U> BuildAlacCookie(const AlacFmtp& fmtp) noexcept {
    std::array<std::byte, 24U> cookie{};
    WriteBigEndian32(cookie, 0U, fmtp.frame_length);
    cookie[4U] = static_cast<std::byte>(fmtp.compatible_version);
    cookie[5U] = static_cast<std::byte>(fmtp.bit_depth);
    cookie[6U] = static_cast<std::byte>(fmtp.pb);
    cookie[7U] = static_cast<std::byte>(fmtp.mb);
    cookie[8U] = static_cast<std::byte>(fmtp.kb);
    cookie[9U] = static_cast<std::byte>(fmtp.channel_count);
    WriteBigEndian16(cookie, 10U, fmtp.max_run);
    WriteBigEndian32(cookie, 12U, fmtp.max_frame_bytes);
    WriteBigEndian32(cookie, 16U, fmtp.average_bit_rate);
    WriteBigEndian32(cookie, 20U, fmtp.sample_rate);
    return cookie;
}

}  // namespace

std::expected<SdpAudioDescription, SdpParseError> ParseSdpAudioSession(
    const std::string_view sdp) {
    if (sdp.empty()) {
        return std::unexpected{SdpParseError::Empty};
    }

    std::optional<std::uint8_t> payload_type;
    std::optional<audio::EncodedAudioFormat> mapped_format;
    std::optional<std::uint8_t> alac_payload_type;
    std::optional<std::pair<std::uint8_t, AlacFmtp>> alac_fmtp;
    std::optional<std::uint32_t> frames_per_packet;
    SdpAudioDescription description;
    std::size_t line_count = 0U;
    std::size_t line_start = 0U;
    while (line_start <= sdp.size()) {
        const auto line_end = sdp.find('\n', line_start);
        const auto line = Trim(sdp.substr(line_start, line_end == std::string_view::npos
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
            const auto fields = SplitWhitespace(line);
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
            const auto space = mapping.find_first_of(" \t");
            if (space == std::string_view::npos) {
                return std::unexpected{SdpParseError::MissingPayloadMapping};
            }
            std::uint16_t mapped_payload = 0U;
            if (!ParseUnsigned(mapping.substr(0U, space), mapped_payload) ||
                mapped_payload > 127U) {
                return std::unexpected{SdpParseError::MissingPayloadMapping};
            }
            const auto encoding_fields = Split(Trim(mapping.substr(space + 1U)), '/');
            if (!encoding_fields.empty() &&
                EqualsAsciiCaseInsensitive(encoding_fields[0], "L16")) {
                if (encoding_fields.size() < 2U) {
                    return std::unexpected{SdpParseError::InvalidClockRate};
                }
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
            } else if (!encoding_fields.empty() &&
                       EqualsAsciiCaseInsensitive(encoding_fields[0], "AppleLossless")) {
                alac_payload_type = static_cast<std::uint8_t>(mapped_payload);
            }
        } else if (line.starts_with("a=fmtp:")) {
            const auto value = line.substr(7U);
            const auto space = value.find_first_of(" \t");
            std::uint16_t mapped_payload = 0U;
            if (space == std::string_view::npos ||
                !ParseUnsigned(value.substr(0U, space), mapped_payload) ||
                mapped_payload > 127U) {
                return std::unexpected{SdpParseError::InvalidCodecConfiguration};
            }
            const auto fmtp = ParseAlacFmtp(Trim(value.substr(space + 1U)));
            if (fmtp.has_value()) {
                alac_fmtp = std::pair{static_cast<std::uint8_t>(mapped_payload), *fmtp};
            }
        } else if (line.starts_with("a=framesize:")) {
            const auto value = line.substr(12U);
            const auto space = value.find_first_of(" \t");
            if (space != std::string_view::npos) {
                std::uint32_t parsed_frames = 0U;
                if (!ParseUnsigned(Trim(value.substr(space + 1U)), parsed_frames) ||
                    parsed_frames == 0U || parsed_frames > 8'192U) {
                    return std::unexpected{SdpParseError::InvalidFramesPerPacket};
                }
                frames_per_packet = parsed_frames;
            }
        } else if (line.starts_with("a=rsaaeskey:")) {
            const auto value = Trim(line.substr(12U));
            if (value.empty() || value.size() > 1'024U) {
                return std::unexpected{SdpParseError::InvalidCodecConfiguration};
            }
            description.encrypted_aes_key = std::string{value};
        } else if (line.starts_with("a=aesiv:")) {
            const auto value = Trim(line.substr(8U));
            if (value.empty() || value.size() > 128U) {
                return std::unexpected{SdpParseError::InvalidCodecConfiguration};
            }
            description.aes_initialization_vector = std::string{value};
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 1U;
    }

    if (!payload_type.has_value()) {
        return std::unexpected{SdpParseError::InvalidMedia};
    }
    if (*payload_type == 10U) {
        description.format = {
            .sample_rate = 44'100U, .channel_count = 2U, .payload_type = 10U};
    } else if (*payload_type == 11U) {
        description.format = {
            .sample_rate = 44'100U, .channel_count = 1U, .payload_type = 11U};
    } else if (alac_payload_type.has_value() && *alac_payload_type == *payload_type) {
        if (!alac_fmtp.has_value() || alac_fmtp->first != *payload_type) {
            return std::unexpected{SdpParseError::InvalidCodecConfiguration};
        }
        const auto& fmtp = alac_fmtp->second;
        description.format = {
            .codec = audio::AudioCodec::AppleLossless,
            .sample_rate = fmtp.sample_rate,
            .channel_count = fmtp.channel_count,
            .payload_type = *payload_type,
            .nominal_frames_per_packet = fmtp.frame_length,
            .codec_config = BuildAlacCookie(fmtp),
            .codec_config_size = 24U,
        };
    } else if (mapped_format.has_value() && mapped_format->payload_type == *payload_type) {
        description.format = *mapped_format;
    } else if (mapped_format.has_value() || alac_payload_type.has_value()) {
        return std::unexpected{SdpParseError::MissingPayloadMapping};
    } else {
        return std::unexpected{SdpParseError::UnsupportedCodec};
    }
    if (frames_per_packet.has_value()) {
        description.format.nominal_frames_per_packet = *frames_per_packet;
    }
    if (description.encrypted_aes_key.has_value() !=
        description.aes_initialization_vector.has_value()) {
        return std::unexpected{SdpParseError::InvalidCodecConfiguration};
    }
    description.format.encrypted = description.encrypted_aes_key.has_value();
    return description;
}

std::expected<audio::EncodedAudioFormat, SdpParseError> ParseSdpAudioDescription(
    const std::string_view sdp) {
    const auto description = ParseSdpAudioSession(sdp);
    if (!description.has_value()) {
        return std::unexpected{description.error()};
    }
    return description->format;
}

}  // namespace airplaywin::protocol
