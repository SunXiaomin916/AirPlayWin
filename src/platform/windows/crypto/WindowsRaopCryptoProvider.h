#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "core/crypto/IRaopCryptoProvider.h"
#include "core/discovery/DiscoveryTypes.h"

namespace airplaywin::windows::crypto {

class WindowsRaopCryptoProvider final : public airplaywin::crypto::IRaopCryptoProvider {
public:
    explicit WindowsRaopCryptoProvider(discovery::DeviceId device_id);
    ~WindowsRaopCryptoProvider() override;

    WindowsRaopCryptoProvider(const WindowsRaopCryptoProvider&) = delete;
    WindowsRaopCryptoProvider& operator=(const WindowsRaopCryptoProvider&) = delete;

    [[nodiscard]] bool Available() const noexcept;
    [[nodiscard]] std::optional<std::string> BuildAppleResponse(
        std::string_view apple_challenge,
        std::string_view local_address) const override;
    [[nodiscard]] std::optional<airplaywin::crypto::RaopAesMaterial> DecryptAesMaterial(
        std::string_view encrypted_key,
        std::string_view initialization_vector) const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace airplaywin::windows::crypto
