#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "AirPlayWinVersion.h"
#include "core/protocol/RtspTypes.h"

namespace airplaywin::protocol {

struct ResponseSpec final {
    int status_code{200};
    std::string_view reason{"OK"};
    std::span<const Header> headers{};
    std::span<const std::byte> body{};
    bool close_connection{false};
    std::string_view server_name{"AirPlayWin/" AIRPLAYWIN_VERSION_STRING};
};

[[nodiscard]] std::vector<std::byte> BuildResponse(const Request& request,
                                                   const ResponseSpec& response);

}  // namespace airplaywin::protocol
