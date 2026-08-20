#include "TestFramework.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/protocol/IncrementalRtspParser.h"

namespace {

[[nodiscard]] std::vector<std::byte> Bytes(const std::string_view text) {
    const auto* const begin = reinterpret_cast<const std::byte*>(text.data());
    return {begin, begin + text.size()};
}

[[nodiscard]] std::vector<std::byte> ReadFixture(const std::string_view name) {
    const std::string_view fixture_root{AIRPLAYWIN_TEST_FIXTURE_DIR};
    std::u8string fixture_root_utf8;
    fixture_root_utf8.reserve(fixture_root.size());
    for (const unsigned char value : fixture_root) {
        fixture_root_utf8.push_back(static_cast<char8_t>(value));
    }
    const std::filesystem::path path =
        std::filesystem::path{fixture_root_utf8} / "rtsp" / name;
    std::ifstream input{path, std::ios::binary};
    APW_EXPECT(input.good());
    const std::string encoded{std::istreambuf_iterator<char>{input},
                              std::istreambuf_iterator<char>{}};
    std::string content;
    content.reserve(encoded.size());
    for (std::size_t index = 0U; index < encoded.size(); ++index) {
        if (encoded[index] == '\\' && index + 1U < encoded.size() &&
            (encoded[index + 1U] == 'r' || encoded[index + 1U] == 'n')) {
            content.push_back(encoded[index + 1U] == 'r' ? '\r' : '\n');
            ++index;
        } else if (encoded[index] != '\r' && encoded[index] != '\n') {
            content.push_back(encoded[index]);
        }
    }
    return Bytes(content);
}

}  // namespace

void TestRtspParser() {
    using airplaywin::protocol::IncrementalRtspParser;
    using airplaywin::protocol::ParseErrorCode;
    using airplaywin::protocol::ProtocolVersion;

    const auto options = ReadFixture("options_request.txt");
    IncrementalRtspParser fragmented;
    const auto first = fragmented.Feed(std::span{options}.first(7U));
    APW_EXPECT(first.requests.empty());
    APW_EXPECT(!first.error.has_value());
    const auto second = fragmented.Feed(std::span{options}.subspan(7U));
    APW_EXPECT(second.requests.size() == 1U);
    APW_EXPECT(second.requests.front().method == "OPTIONS");
    APW_EXPECT(second.requests.front().target == "*");
    APW_EXPECT(second.requests.front().version == ProtocolVersion::Rtsp10);
    APW_EXPECT(second.requests.front().HeaderValue("cseq") == "1");
    APW_EXPECT(fragmented.BufferedBytes() == 0U);

    auto pipelined_bytes = options;
    pipelined_bytes.insert(pipelined_bytes.end(), options.begin(), options.end());
    IncrementalRtspParser pipelined;
    const auto pipeline = pipelined.Feed(pipelined_bytes);
    APW_EXPECT(pipeline.requests.size() == 2U);
    APW_EXPECT(!pipeline.error.has_value());

    auto binary = Bytes("POST /binary HTTP/1.1\r\nContent-Length: 4\r\n\r\n");
    binary.push_back(std::byte{'A'});
    binary.push_back(std::byte{'B'});
    binary.push_back(std::byte{0});
    binary.push_back(std::byte{'C'});
    IncrementalRtspParser binary_parser;
    const auto binary_batch = binary_parser.Feed(binary);
    APW_EXPECT(binary_batch.requests.size() == 1U);
    APW_EXPECT(binary_batch.requests.front().body.size() == 4U);
    APW_EXPECT(binary_batch.requests.front().body[2] == std::byte{0});

    IncrementalRtspParser duplicate_length;
    const auto duplicate = duplicate_length.Feed(Bytes(
        "OPTIONS * RTSP/1.0\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n"));
    APW_EXPECT(duplicate.error.has_value());
    APW_EXPECT(duplicate.error->code == ParseErrorCode::InvalidContentLength);
    APW_EXPECT(duplicate_length.Failed());

    IncrementalRtspParser chunked;
    const auto chunked_result = chunked.Feed(
        Bytes("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"));
    APW_EXPECT(chunked_result.error.has_value());
    APW_EXPECT(chunked_result.error->code == ParseErrorCode::UnsupportedTransferEncoding);

    IncrementalRtspParser oversized{
        airplaywin::protocol::ParserLimits{.max_body_bytes = 8U}};
    const auto oversized_result =
        oversized.Feed(Bytes("POST / HTTP/1.1\r\nContent-Length: 9\r\n\r\n"));
    APW_EXPECT(oversized_result.error.has_value());
    APW_EXPECT(oversized_result.error->code == ParseErrorCode::BodyTooLarge);
}
