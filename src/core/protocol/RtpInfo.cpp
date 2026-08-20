#include "core/protocol/RtpInfo.h"

#include <charconv>
#include <cstdint>

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

template <typename Integer>
[[nodiscard]] bool ParseUnsigned(const std::string_view value, Integer& parsed) noexcept {
    if (value.empty() || value.front() == '+' || value.front() == '-') {
        return false;
    }
    const auto result =
        std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

}  // namespace

std::optional<transport::AudioTimelineAnchor> ParseRtpInfo(
    const std::string_view value) noexcept {
    if (value.empty() || value.find(',') != std::string_view::npos) {
        return std::nullopt;
    }

    transport::AudioTimelineAnchor anchor;
    bool saw_sequence = false;
    bool saw_timestamp = false;
    std::size_t offset = 0U;
    while (offset <= value.size()) {
        const auto separator = value.find(';', offset);
        const auto field = Trim(value.substr(
            offset, separator == std::string_view::npos ? std::string_view::npos
                                                        : separator - offset));
        if (field.empty()) {
            return std::nullopt;
        }
        const auto equals = field.find('=');
        if (equals == std::string_view::npos) {
            return std::nullopt;
        }
        const auto name = Trim(field.substr(0U, equals));
        const auto field_value = Trim(field.substr(equals + 1U));
        if (EqualsAsciiCaseInsensitive(name, "seq")) {
            std::uint16_t parsed = 0U;
            if (saw_sequence || !ParseUnsigned(field_value, parsed)) {
                return std::nullopt;
            }
            saw_sequence = true;
            anchor.sequence_number = parsed;
        } else if (EqualsAsciiCaseInsensitive(name, "rtptime")) {
            std::uint32_t parsed = 0U;
            if (saw_timestamp || !ParseUnsigned(field_value, parsed)) {
                return std::nullopt;
            }
            saw_timestamp = true;
            anchor.rtp_timestamp = parsed;
        } else if (EqualsAsciiCaseInsensitive(name, "url")) {
            if (field_value.empty()) {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }

        if (separator == std::string_view::npos) {
            break;
        }
        offset = separator + 1U;
    }
    return anchor.IsEmpty() ? std::nullopt
                            : std::optional<transport::AudioTimelineAnchor>{anchor};
}

}  // namespace airplaywin::protocol
