#include "core/protocol/RtspTransport.h"

#include <charconv>
#include <optional>

#include "core/protocol/RtspTypes.h"

namespace airplaywin::protocol {

namespace {

[[nodiscard]] std::string_view Trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1U);
    }
    return value;
}

[[nodiscard]] bool ParsePort(const std::string_view text, std::uint16_t& port) noexcept {
    std::uint32_t value = 0U;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        value == 0U || value > 65'535U) {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
    return true;
}

}  // namespace

std::expected<RtspTransportParameters, TransportParseError> ParseRecordTransport(
    const std::string_view header) noexcept {
    if (header.empty()) {
        return std::unexpected{TransportParseError::Empty};
    }
    const auto first_separator = header.find(';');
    const auto protocol = Trim(header.substr(0U, first_separator));
    if (!EqualsAsciiCaseInsensitive(protocol, "RTP/AVP/UDP") &&
        !EqualsAsciiCaseInsensitive(protocol, "RTP/AVP")) {
        return std::unexpected{TransportParseError::UnsupportedProtocol};
    }
    RtspTransportParameters parameters;
    bool saw_unicast = false;
    bool saw_control_port = false;
    bool saw_timing_port = false;
    std::size_t begin = first_separator == std::string_view::npos ? header.size()
                                                                  : first_separator + 1U;
    while (begin < header.size()) {
        const auto end = header.find(';', begin);
        const auto token = Trim(header.substr(begin, end == std::string_view::npos
                                                         ? end
                                                         : end - begin));
        if (EqualsAsciiCaseInsensitive(token, "unicast")) {
            if (saw_unicast) {
                return std::unexpected{TransportParseError::DuplicateParameter};
            }
            saw_unicast = true;
        } else if (EqualsAsciiCaseInsensitive(token, "multicast")) {
            return std::unexpected{TransportParseError::MulticastUnsupported};
        } else {
            const auto equals = token.find('=');
            if (equals == std::string_view::npos || equals == 0U) {
                return std::unexpected{TransportParseError::MalformedParameter};
            }
            const auto name = Trim(token.substr(0U, equals));
            auto value = Trim(token.substr(equals + 1U));
            if (EqualsAsciiCaseInsensitive(name, "mode")) {
                if (value.size() >= 2U && value.front() == '"' && value.back() == '"') {
                    value = value.substr(1U, value.size() - 2U);
                }
                if (!EqualsAsciiCaseInsensitive(value, "record")) {
                    return std::unexpected{TransportParseError::InvalidMode};
                }
            } else if (EqualsAsciiCaseInsensitive(name, "control_port")) {
                if (saw_control_port) {
                    return std::unexpected{TransportParseError::DuplicateParameter};
                }
                if (!ParsePort(value, parameters.client_control_port)) {
                    return std::unexpected{TransportParseError::InvalidPort};
                }
                saw_control_port = true;
            } else if (EqualsAsciiCaseInsensitive(name, "timing_port")) {
                if (saw_timing_port) {
                    return std::unexpected{TransportParseError::DuplicateParameter};
                }
                if (!ParsePort(value, parameters.client_timing_port)) {
                    return std::unexpected{TransportParseError::InvalidPort};
                }
                saw_timing_port = true;
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    if (!saw_unicast) {
        return std::unexpected{TransportParseError::MalformedParameter};
    }
    return parameters;
}

std::string BuildRecordTransportResponse(const std::uint16_t server_audio_port,
                                         const std::uint16_t server_control_port,
                                         const std::uint16_t server_timing_port) {
    return "RTP/AVP/UDP;unicast;mode=record;server_port=" +
           std::to_string(server_audio_port) + ";control_port=" +
           std::to_string(server_control_port) + ";timing_port=" +
           std::to_string(server_timing_port);
}

}  // namespace airplaywin::protocol
