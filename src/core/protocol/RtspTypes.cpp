#include "core/protocol/RtspTypes.h"

#include <algorithm>
#include <cctype>

namespace airplaywin::protocol {

namespace {

[[nodiscard]] char LowerAscii(const char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

}  // namespace

bool EqualsAsciiCaseInsensitive(const std::string_view left,
                                const std::string_view right) noexcept {
    return left.size() == right.size() &&
           std::ranges::equal(left, right, [](const char lhs, const char rhs) {
               return LowerAscii(lhs) == LowerAscii(rhs);
           });
}

std::optional<std::string_view> Request::HeaderValue(const std::string_view name) const noexcept {
    for (const auto& header : headers) {
        if (EqualsAsciiCaseInsensitive(header.name, name)) {
            return header.value;
        }
    }
    return std::nullopt;
}

std::string_view Request::BodyText() const noexcept {
    return {reinterpret_cast<const char*>(body.data()), body.size()};
}

std::string_view ToString(const ProtocolVersion version) noexcept {
    switch (version) {
        case ProtocolVersion::Rtsp10:
            return "RTSP/1.0";
        case ProtocolVersion::Http10:
            return "HTTP/1.0";
        case ProtocolVersion::Http11:
            return "HTTP/1.1";
    }
    return "RTSP/1.0";
}

}  // namespace airplaywin::protocol
