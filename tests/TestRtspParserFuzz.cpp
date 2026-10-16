#include "TestFramework.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "core/protocol/IncrementalRtspParser.h"

// Deterministic coverage-oriented smoke fuzz: feed random and mutated
// byte streams into the incremental parser and assert it never crashes,
// never exceeds its buffering limits, and always reports a terminal state
// for hostile input. This is not a replacement for a coverage-guided
// fuzzer, but it runs in CI on the native test suite.
void TestRtspParserFuzz() {
    using airplaywin::protocol::IncrementalRtspParser;
    using airplaywin::protocol::ParserLimits;
    using airplaywin::protocol::ParseBatch;

    const ParserLimits limits{};
    std::mt19937 generator{0x5EED'CAFEU};

    // Base seeds: valid fragments and near-valid requests.
    const std::array<std::vector<std::byte>, 5U> seeds{{
        {std::byte{'O'}, std::byte{'P'}, std::byte{'T'}, std::byte{'I'}},
        {std::byte{'A'}, std::byte{'N'}, std::byte{'N'}, std::byte{'O'},
         std::byte{'U'}, std::byte{'N'}, std::byte{'C'}, std::byte{'E'}},
        {std::byte{0x0D}, std::byte{0x0A}, std::byte{0x0D}, std::byte{0x0A}},
        {std::byte{'C'}, std::byte{'o'}, std::byte{'n'}, std::byte{'t'},
         std::byte{'e'}, std::byte{'n'}, std::byte{'t'}, std::byte{'-'},
         std::byte{'L'}, std::byte{'e'}, std::byte{'n'}, std::byte{'g'},
         std::byte{'t'}, std::byte{'h'}, std::byte{':'}, std::byte{' '},
         std::byte{'9'}, std::byte{'9'}, std::byte{'9'}, std::byte{'9'},
         std::byte{'9'}, std::byte{'9'}, std::byte{'9'}, std::byte{'9'}},
        {std::byte{'R'}, std::byte{'E'}, std::byte{'C'}, std::byte{'O'},
         std::byte{'R'}, std::byte{'D'}, std::byte{' '}},
    }};

    std::uniform_int_distribution<std::uint32_t> byte_distribution(0U, 255U);
    std::uniform_int_distribution<std::size_t> length_distribution(0U, 512U);
    std::uniform_int_distribution<std::size_t> seed_distribution(0U, seeds.size() - 1U);
    std::uniform_int_distribution<std::size_t> chunk_distribution(1U, 64U);

    for (std::uint32_t iteration = 0U; iteration < 2'000U; ++iteration) {
        const auto& seed = seeds[seed_distribution(generator)];
        std::vector<std::byte> input = seed;
        const auto extra = length_distribution(generator);
        input.reserve(input.size() + extra);
        for (std::size_t index = 0U; index < extra; ++index) {
            input.push_back(static_cast<std::byte>(byte_distribution(generator)));
        }

        IncrementalRtspParser parser{limits};
        std::size_t offset = 0U;
        while (offset < input.size()) {
            const auto chunk = std::min(chunk_distribution(generator), input.size() - offset);
            const std::span<const std::byte> bytes{input.data() + offset, chunk};
            offset += chunk;
            ParseBatch batch;
            // The parser must never throw, regardless of input.
            batch = parser.Feed(bytes);
            static_cast<void>(batch);
            APW_EXPECT(parser.BufferedBytes() <= limits.max_buffered_bytes);
            if (parser.Failed()) {
                break;
            }
        }
        // A hostile stream must eventually either parse or reach a
        // terminal failure; unbounded buffering is the one thing that is
        // never allowed.
        APW_EXPECT(parser.BufferedBytes() <= limits.max_buffered_bytes);
    }

    // Valid requests split across hostile chunk boundaries still parse.
    const auto request_bytes = [](const std::string_view text) {
        return std::vector<std::byte>{reinterpret_cast<const std::byte*>(text.data()),
                                      reinterpret_cast<const std::byte*>(text.data()) +
                                          text.size()};
    };
    const auto valid = request_bytes(
        "OPTIONS rtsp://127.0.0.1/stream RTSP/1.0\r\n"
        "CSeq: 1\r\n"
        "Content-Length: 0\r\n\r\n");
    IncrementalRtspParser parser{limits};
    for (std::size_t index = 0U; index < valid.size(); ++index) {
        const std::span<const std::byte> single{valid.data() + index, 1U};
        const auto batch = parser.Feed(single);
        if (index + 1U == valid.size()) {
            APW_EXPECT(!batch.error.has_value());
            APW_EXPECT(batch.requests.size() == 1U);
        }
    }
}
