#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace airplaywin::protocol {

enum class ProtocolVersion : std::uint8_t {
    Rtsp10,
    Http10,
    Http11,
};

struct Header final {
    std::string name{};
    std::string value{};
};

struct Request final {
    std::string method{};
    std::string target{};
    ProtocolVersion version{ProtocolVersion::Rtsp10};
    std::vector<Header> headers{};
    std::vector<std::byte> body{};

    [[nodiscard]] std::optional<std::string_view> HeaderValue(
        std::string_view name) const noexcept;
    [[nodiscard]] std::string_view BodyText() const noexcept;
};

enum class ParseErrorCode : std::uint8_t {
    InvalidStartLine,
    UnsupportedVersion,
    InvalidHeader,
    TooManyHeaders,
    HeaderSectionTooLarge,
    InvalidContentLength,
    BodyTooLarge,
    UnsupportedTransferEncoding,
    BufferLimitExceeded,
};

struct ParseError final {
    ParseErrorCode code{ParseErrorCode::InvalidStartLine};
    std::string detail{};
};

[[nodiscard]] bool EqualsAsciiCaseInsensitive(std::string_view left,
                                              std::string_view right) noexcept;
[[nodiscard]] std::string_view ToString(ProtocolVersion version) noexcept;

}  // namespace airplaywin::protocol
