#include "core/protocol/RtspResponseBuilder.h"

#include <charconv>
#include <string>

namespace airplaywin::protocol {

namespace {

void Append(std::vector<std::byte>& output, const std::string_view text) {
    const auto* const begin = reinterpret_cast<const std::byte*>(text.data());
    output.insert(output.end(), begin, begin + text.size());
}

[[nodiscard]] bool SafeHeader(const Header& header) noexcept {
    return !header.name.empty() && header.name.find_first_of("\r\n:") == std::string::npos &&
           header.value.find_first_of("\r\n") == std::string::npos;
}

}  // namespace

std::vector<std::byte> BuildResponse(const Request& request, const ResponseSpec& response) {
    std::vector<std::byte> output;
    output.reserve(256U + response.body.size());
    Append(output, ToString(request.version));
    Append(output, " ");
    char status[16]{};
    const auto status_result = std::to_chars(status, status + sizeof(status), response.status_code);
    Append(output, {status, static_cast<std::size_t>(status_result.ptr - status)});
    Append(output, " ");
    Append(output, response.reason);
    Append(output, "\r\n");
    if (const auto cseq = request.HeaderValue("CSeq"); cseq.has_value()) {
        Append(output, "CSeq: ");
        Append(output, *cseq);
        Append(output, "\r\n");
    }
    Append(output, "Server: ");
    Append(output, response.server_name);
    Append(output, "\r\n");
    for (const auto& header : response.headers) {
        if (SafeHeader(header) && !EqualsAsciiCaseInsensitive(header.name, "Content-Length") &&
            !EqualsAsciiCaseInsensitive(header.name, "CSeq") &&
            !EqualsAsciiCaseInsensitive(header.name, "Server")) {
            Append(output, header.name);
            Append(output, ": ");
            Append(output, header.value);
            Append(output, "\r\n");
        }
    }
    if (response.close_connection) {
        Append(output, "Connection: close\r\n");
    }
    Append(output, "Content-Length: ");
    char length[32]{};
    const auto length_result = std::to_chars(length, length + sizeof(length), response.body.size());
    Append(output, {length, static_cast<std::size_t>(length_result.ptr - length)});
    Append(output, "\r\n\r\n");
    output.insert(output.end(), response.body.begin(), response.body.end());
    return output;
}

}  // namespace airplaywin::protocol
