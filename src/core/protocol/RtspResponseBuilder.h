#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/protocol/RtspTypes.h"

namespace airplaywin::protocol {

struct ResponseSpec final {
    int status_code{200};
    std::string_view reason{"OK"};
    std::span<const Header> headers{};
    std::span<const std::byte> body{};
    bool close_connection{false};
};

[[nodiscard]] std::vector<std::byte> BuildResponse(const Request& request,
                                                   const ResponseSpec& response);

}  // namespace airplaywin::protocol
