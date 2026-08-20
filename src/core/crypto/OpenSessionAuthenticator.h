#pragma once

#include "core/crypto/ISessionAuthenticator.h"

namespace airplaywin::crypto {

class OpenSessionAuthenticator final : public ISessionAuthenticator {
public:
    [[nodiscard]] AuthorizationDecision Authorize(
        const protocol::Request& request) const override;
};

}  // namespace airplaywin::crypto
