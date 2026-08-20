#include "core/crypto/OpenSessionAuthenticator.h"

namespace airplaywin::crypto {

AuthorizationDecision OpenSessionAuthenticator::Authorize(
    const protocol::Request& request) const {
    static_cast<void>(request);
    return AuthorizationDecision{.result = AuthorizationResult::Allowed};
}

}  // namespace airplaywin::crypto
