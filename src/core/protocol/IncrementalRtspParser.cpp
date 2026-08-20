#include "core/protocol/IncrementalRtspParser.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <string_view>

namespace airplaywin::protocol {

namespace {

constexpr std::string_view kHeaderTerminator{"\r\n\r\n"};

[[nodiscard]] bool IsTokenCharacter(const char value) noexcept {
    if (value <= 0x20 || value >= 0x7f) {
        return false;
    }
    constexpr std::string_view kSeparators{"()<>@,;:\\\"/[]?={}"};
    return kSeparators.find(value) == std::string_view::npos;
}

[[nodiscard]] std::string_view TrimOptionalWhitespace(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1U);
    }
    return value;
}

[[nodiscard]] std::optional<ProtocolVersion> ParseVersion(const std::string_view value) noexcept {
    if (value == "RTSP/1.0") {
        return ProtocolVersion::Rtsp10;
    }
    if (value == "HTTP/1.0") {
        return ProtocolVersion::Http10;
    }
    if (value == "HTTP/1.1") {
        return ProtocolVersion::Http11;
    }
    return std::nullopt;
}

[[nodiscard]] ParseError Error(const ParseErrorCode code, std::string detail) {
    return ParseError{.code = code, .detail = std::move(detail)};
}

}  // namespace

IncrementalRtspParser::IncrementalRtspParser(const ParserLimits limits) : limits_(limits) {
    const auto reserve = std::min(limits_.max_header_section_bytes, limits_.max_buffered_bytes);
    buffer_.reserve(reserve);
}

ParseBatch IncrementalRtspParser::Feed(const std::span<const std::byte> bytes) {
    ParseBatch batch;
    if (failed_) {
        batch.error = Error(ParseErrorCode::BufferLimitExceeded,
                            "parser is terminal until Reset is called");
        return batch;
    }
    if (bytes.size() > limits_.max_buffered_bytes -
                           std::min(buffer_.size(), limits_.max_buffered_bytes)) {
        failed_ = true;
        buffer_.clear();
        batch.error = Error(ParseErrorCode::BufferLimitExceeded,
                            "incremental input exceeded the parser buffer limit");
        return batch;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());

    while (!buffer_.empty() && batch.requests.size() < limits_.max_messages_per_feed) {
        Request request;
        std::size_t consumed = 0U;
        const auto error = ParseOne(request, consumed);
        if (error.has_value()) {
            failed_ = true;
            buffer_.clear();
            batch.error = error;
            break;
        }
        if (consumed == 0U) {
            break;
        }
        batch.requests.push_back(std::move(request));
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
    if (!buffer_.empty() && batch.requests.size() == limits_.max_messages_per_feed) {
        failed_ = true;
        buffer_.clear();
        batch.error = Error(ParseErrorCode::BufferLimitExceeded,
                            "too many pipelined requests arrived in one input batch");
    }
    return batch;
}

std::optional<ParseError> IncrementalRtspParser::ParseOne(Request& request,
                                                          std::size_t& consumed) const {
    consumed = 0U;
    const std::string_view text{reinterpret_cast<const char*>(buffer_.data()), buffer_.size()};
    const auto terminator = text.find(kHeaderTerminator);
    if (terminator == std::string_view::npos) {
        if (text.size() > limits_.max_header_section_bytes) {
            return Error(ParseErrorCode::HeaderSectionTooLarge,
                         "request head exceeded the configured limit");
        }
        return std::nullopt;
    }
    const auto header_bytes = terminator + kHeaderTerminator.size();
    if (text.substr(0U, header_bytes).find('\0') != std::string_view::npos) {
        return Error(ParseErrorCode::InvalidHeader, "NUL is not valid in the request head");
    }
    if (header_bytes > limits_.max_header_section_bytes) {
        return Error(ParseErrorCode::HeaderSectionTooLarge,
                     "request head exceeded the configured limit");
    }

    const auto first_line_end = text.find("\r\n");
    if (first_line_end == std::string_view::npos || first_line_end > limits_.max_start_line_bytes) {
        return Error(ParseErrorCode::InvalidStartLine, "invalid or oversized request line");
    }
    const auto start_line = text.substr(0U, first_line_end);
    const auto first_space = start_line.find(' ');
    const auto second_space = first_space == std::string_view::npos
                                  ? std::string_view::npos
                                  : start_line.find(' ', first_space + 1U);
    if (first_space == 0U || second_space == std::string_view::npos ||
        second_space == first_space + 1U || start_line.find(' ', second_space + 1U) !=
                                                 std::string_view::npos) {
        return Error(ParseErrorCode::InvalidStartLine, "request line must contain three fields");
    }
    const auto method = start_line.substr(0U, first_space);
    if (!std::ranges::all_of(method, IsTokenCharacter)) {
        return Error(ParseErrorCode::InvalidStartLine, "request method is not an ASCII token");
    }
    const auto target = start_line.substr(first_space + 1U, second_space - first_space - 1U);
    if (target.empty() || std::ranges::any_of(target, [](const char value) {
            const auto byte = static_cast<unsigned char>(value);
            return byte <= 0x20U || byte == 0x7fU;
        })) {
        return Error(ParseErrorCode::InvalidStartLine,
                     "request target contains an invalid character");
    }
    const auto version = ParseVersion(start_line.substr(second_space + 1U));
    if (!version.has_value()) {
        return Error(ParseErrorCode::UnsupportedVersion, "unsupported RTSP/HTTP version");
    }
    request.method.assign(method);
    request.target.assign(target);
    request.version = *version;

    std::size_t content_length = 0U;
    bool has_content_length = false;
    std::size_t line_start = first_line_end + 2U;
    while (line_start < terminator) {
        const auto line_end = text.find("\r\n", line_start);
        if (line_end == std::string_view::npos || line_end > terminator) {
            return Error(ParseErrorCode::InvalidHeader, "header line is not CRLF terminated");
        }
        const auto line = text.substr(line_start, line_end - line_start);
        if (line.size() > limits_.max_header_line_bytes || line.empty() ||
            line.front() == ' ' || line.front() == '\t') {
            return Error(ParseErrorCode::InvalidHeader,
                         "invalid, folded, or oversized header line");
        }
        if (request.headers.size() >= limits_.max_header_count) {
            return Error(ParseErrorCode::TooManyHeaders, "too many request headers");
        }
        const auto colon = line.find(':');
        if (colon == 0U || colon == std::string_view::npos) {
            return Error(ParseErrorCode::InvalidHeader, "header is missing a valid name");
        }
        const auto name = line.substr(0U, colon);
        if (!std::ranges::all_of(name, IsTokenCharacter)) {
            return Error(ParseErrorCode::InvalidHeader, "header name is not an ASCII token");
        }
        const auto value = TrimOptionalWhitespace(line.substr(colon + 1U));
        if (std::ranges::any_of(value, [](const char character) {
                const auto byte = static_cast<unsigned char>(character);
                return (byte < 0x20U && byte != static_cast<unsigned char>('\t')) ||
                       byte == 0x7fU;
            })) {
            return Error(ParseErrorCode::InvalidHeader,
                         "header value contains a prohibited control character");
        }
        if (EqualsAsciiCaseInsensitive(name, "Transfer-Encoding")) {
            return Error(ParseErrorCode::UnsupportedTransferEncoding,
                         "RTSP chunked transfer coding is unsupported");
        }
        if (EqualsAsciiCaseInsensitive(name, "Content-Length")) {
            if (has_content_length || value.empty() || value.front() == '+' ||
                value.front() == '-') {
                return Error(ParseErrorCode::InvalidContentLength,
                             "Content-Length must appear once as an unsigned decimal value");
            }
            const auto result = std::from_chars(value.data(), value.data() + value.size(),
                                                content_length, 10);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
                return Error(ParseErrorCode::InvalidContentLength,
                             "Content-Length is not an unsigned decimal value");
            }
            has_content_length = true;
        }
        request.headers.push_back(Header{std::string{name}, std::string{value}});
        line_start = line_end + 2U;
    }

    if (content_length > limits_.max_body_bytes) {
        return Error(ParseErrorCode::BodyTooLarge, "request body exceeded the configured limit");
    }
    if (content_length > limits_.max_buffered_bytes - header_bytes) {
        return Error(ParseErrorCode::BufferLimitExceeded,
                     "declared request size exceeded the parser buffer limit");
    }
    const auto message_bytes = header_bytes + content_length;
    if (buffer_.size() < message_bytes) {
        return std::nullopt;
    }
    request.body.assign(buffer_.begin() + static_cast<std::ptrdiff_t>(header_bytes),
                        buffer_.begin() + static_cast<std::ptrdiff_t>(message_bytes));
    consumed = message_bytes;
    return std::nullopt;
}

bool IncrementalRtspParser::Failed() const noexcept {
    return failed_;
}

std::size_t IncrementalRtspParser::BufferedBytes() const noexcept {
    return buffer_.size();
}

void IncrementalRtspParser::Reset() noexcept {
    failed_ = false;
    buffer_.clear();
}

}  // namespace airplaywin::protocol
