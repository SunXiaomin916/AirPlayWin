#pragma once

#include <cstdint>
#include <string>

#include "core/protocol/RtspTypes.h"

namespace airplaywin::crypto {

enum class AuthorizationResult : std::uint8_t {
    Allowed,
    AuthenticationRequired,
    Rejected,
};

struct AuthorizationDecision final {
    AuthorizationResult result{AuthorizationResult::Rejected};
    std::string challenge{};
};

class ISessionAuthenticator {
public:
    virtual ~ISessionAuthenticator() = default;

    [[nodiscard]] virtual AuthorizationDecision Authorize(
        const protocol::Request& request) const = 0;
};

}  // namespace airplaywin::crypto
