#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace airplaywin::crypto {

struct RaopAesMaterial final {
    std::array<std::byte, 16U> key{};
    std::array<std::byte, 16U> iv{};
};

class IRaopCryptoProvider {
public:
    virtual ~IRaopCryptoProvider() = default;

    [[nodiscard]] virtual std::optional<std::string> BuildAppleResponse(
        std::string_view apple_challenge,
        std::string_view local_address) const = 0;
    [[nodiscard]] virtual std::optional<RaopAesMaterial> DecryptAesMaterial(
        std::string_view encrypted_key,
        std::string_view initialization_vector) const = 0;
};

}  // namespace airplaywin::crypto
