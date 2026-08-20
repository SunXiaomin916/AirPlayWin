#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "core/protocol/RtspTypes.h"

namespace airplaywin::protocol {

struct ParserLimits final {
    std::size_t max_start_line_bytes{4U * 1'024U};
    std::size_t max_header_line_bytes{8U * 1'024U};
    std::size_t max_header_section_bytes{64U * 1'024U};
    std::size_t max_header_count{100U};
    std::size_t max_body_bytes{1U * 1'024U * 1'024U};
    std::size_t max_buffered_bytes{2U * 1'024U * 1'024U};
    std::size_t max_messages_per_feed{16U};
};

struct ParseBatch final {
    std::vector<Request> requests{};
    std::optional<ParseError> error{};
};

class IncrementalRtspParser final {
public:
    explicit IncrementalRtspParser(ParserLimits limits = {});

    [[nodiscard]] ParseBatch Feed(std::span<const std::byte> bytes);
    [[nodiscard]] bool Failed() const noexcept;
    [[nodiscard]] std::size_t BufferedBytes() const noexcept;
    void Reset() noexcept;

private:
    [[nodiscard]] std::optional<ParseError> ParseOne(Request& request,
                                                     std::size_t& consumed) const;

    ParserLimits limits_{};
    std::vector<std::byte> buffer_{};
    bool failed_{false};
};

}  // namespace airplaywin::protocol
