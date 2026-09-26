#include "platform/windows/crypto/WindowsRaopCryptoProvider.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace airplaywin::windows::crypto {
namespace {

// Publicly documented legacy AirPort Express RAOP compatibility key. It is used only for
// the classic Apple-Challenge proof and is not a per-installation identity or user secret.
constexpr char kLegacyAirportPrivateKey[] = R"pem(-----BEGIN RSA PRIVATE KEY-----
MIIEpQIBAAKCAQEA59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Qg90u3sG/1CUt
wC5vOYvfDmFI6oSFXi5ELabWJmT2dKHzBJKa3k9ok+8t9ucRqMd6DZHJ2YCCLlDRKSKv6kDqnw4U
wPdpOMXziC/AMj3Z/lUVX1G7WSHCAWKf1zNS1eLvqr+boEjXuBOitnZ/bDzPHrTOZz0Dew0uowxf
/+sG+NCK3eQJVxqcaJ/vEHKIVd2M+5qL71yJQ+87X6oV3eaYvt3zWZYD6z5vYTcrtij2VZ9Zmni/
UAaHqn9JdsBWLUEpVviYnhimNVvYFZeCXg/IdTQ+x4IRdiXNv5hEewIDAQABAoIBAQDl8Axy9XfW
BLmkzkEiqoSwF0PsmVrPzH9KsnwLGH+QZlvjWd8SWYGN7u1507HvhF5N3drJoVU3O14nDY4TFQAa
LlJ9VM35AApXaLyY1ERrN7u9ALKd2LUwYhM7Km539O4yUFYikE2nIPscEsA5ltpxOgUGCY7b7ez5
NtD6nL1ZKauw7aNXmVAvmJTcuPxWmoktF3gDJKK2wxZuNGcJE0uFQEG4Z3BrWP7yoNuSK3dii2jm
lpPHr0O/KnPQtzI3eguhe0TwUem/eYSdyzMyVx/YpwkzwtYL3sR5k0o9rKQLtvLzfAqdBxBurciz
aaA/L0HIgAmOit1GJA2saMxTVPNhAoGBAPfgv1oeZxgxmotiCcMXFEQEWflzhWYTsXrhUIuz5jFu
a39GLS99ZEErhLdrwj8rDDViRVJ5skOp9zFvlYAHs0xh92ji1E7V/ysnKBfsMrPkk5KSKPrnjndM
oPdevWnVkgJ5jxFuNgxkOLMuG9i53B4yMvDTCRiIPMQ++N2iLDaRAoGBAO9v//mU8eVkQaoANf0Z
oMjW8CN4xwWA2cSEIHkd9AfFkftuv8oyLDCG3ZAf0vrhrrtkrfa7ef+AUb69DNggq4mHQAYBp7L+
k5DKzJrKuO0r+R0YbY9pZD1+/g9dVt91d6LQNepUE/yY2PP5CNoFmjedpLHMOPFdVgqDzDFxU8hL
AoGBANDrr7xAJbqBjHVwIzQ4To9pb4BNeqDndk5Qe7fT3+/H1njGaC0/rXE0Qb7q5ySgnsCb3DvA
cJyRM9SJ7OKlGt0FMSdJD5KG0XPIpAVNwgpXXH5MDJg09KHeh0kXo+QA6viFBi21y340NonnEfdf
54PX4ZGS/Xac1UK+pLkBB+zRAoGAf0AY3H3qKS2lMEI4bzEFoHeK3G895pDaK3TFBVmD7fV0Zhov
17fegFPMwOII8MisYm9ZfT2Z0s5Ro3s5rkt+nvLAdfC/PYPKzTLalpGSwomSNYJcB9HNMlmhkGzc
1JnLYT4iyUyx6pcZBmCd8bD0iwY/FzcgNDaUmbX9+XDvRA0CgYEAkE7pIPlE71qvfJQgoA9em0gI
LAuE4Pu13aKiJnfft7hIjbK+5kyb3TysZvoyDnb3HOKvInK7vXbKuU4ISgxB2bB3HcYzQMGsz1qJ
2gG0N5hvJpzwwhbhXqFKA4zaaSrw622wDniAK5MlIE0tIAKKP4yxNGjoD2QYjhBGuhvkWKY=
-----END RSA PRIVATE KEY-----)pem";

[[nodiscard]] bool NtSucceeded(const NTSTATUS status) noexcept {
    return status >= 0;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> DecodeBase64(
    const std::string_view text,
    const DWORD flags) {
    if (text.empty() || text.size() > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())) {
        return std::nullopt;
    }
    DWORD byte_count = 0U;
    if (CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), flags, nullptr,
                             &byte_count, nullptr, nullptr) == FALSE ||
        byte_count == 0U) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(byte_count);
    if (CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), flags, bytes.data(),
                             &byte_count, nullptr, nullptr) == FALSE) {
        return std::nullopt;
    }
    bytes.resize(byte_count);
    return bytes;
}

[[nodiscard]] std::optional<std::string> EncodeBase64(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.empty() ||
        bytes.size() > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())) {
        return std::nullopt;
    }
    DWORD character_count = 0U;
    constexpr DWORD kFlags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    if (CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()), kFlags, nullptr,
                             &character_count) == FALSE ||
        character_count <= 1U) {
        return std::nullopt;
    }
    std::string encoded(character_count, '\0');
    if (CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()), kFlags,
                             encoded.data(), &character_count) == FALSE) {
        return std::nullopt;
    }
    encoded.resize(character_count == 0U ? 0U : character_count - 1U);
    while (!encoded.empty() && encoded.back() == '=') {
        encoded.pop_back();
    }
    return encoded;
}

}  // namespace

class WindowsRaopCryptoProvider::Impl final {
public:
    explicit Impl(const discovery::DeviceId device_id) : device_id_(device_id) {
        const auto der = DecodeBase64(kLegacyAirportPrivateKey, CRYPT_STRING_BASE64HEADER);
        if (!der.has_value()) {
            return;
        }

        BYTE* legacy_blob = nullptr;
        DWORD legacy_blob_bytes = 0U;
        if (CryptDecodeObjectEx(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                PKCS_RSA_PRIVATE_KEY, der->data(),
                                static_cast<DWORD>(der->size()), CRYPT_DECODE_ALLOC_FLAG,
                                nullptr, &legacy_blob, &legacy_blob_bytes) == FALSE) {
            return;
        }
        const auto release_blob = [&legacy_blob]() noexcept {
            if (legacy_blob != nullptr) {
                static_cast<void>(LocalFree(legacy_blob));
            }
        };

        if (!NtSucceeded(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_RSA_ALGORITHM,
                                                      nullptr, 0U))) {
            release_blob();
            algorithm_ = nullptr;
            return;
        }
        const auto import_status = BCryptImportKeyPair(
            algorithm_, nullptr, LEGACY_RSAPRIVATE_BLOB, &key_, legacy_blob,
            legacy_blob_bytes, 0U);
        release_blob();
        if (!NtSucceeded(import_status)) {
            key_ = nullptr;
        }
    }

    ~Impl() {
        if (key_ != nullptr) {
            static_cast<void>(BCryptDestroyKey(key_));
        }
        if (algorithm_ != nullptr) {
            static_cast<void>(BCryptCloseAlgorithmProvider(algorithm_, 0U));
        }
    }

    [[nodiscard]] bool Available() const noexcept { return key_ != nullptr; }

    [[nodiscard]] std::optional<std::string> BuildAppleResponse(
        const std::string_view challenge,
        const std::string_view local_address) const {
        if (!Available() || challenge.empty() || challenge.size() > 128U ||
            local_address.empty() || local_address.size() > 64U) {
            return std::nullopt;
        }

        std::string padded_challenge{challenge};
        padded_challenge.append((4U - (padded_challenge.size() % 4U)) % 4U, '=');
        const auto decoded = DecodeBase64(padded_challenge, CRYPT_STRING_BASE64);
        if (!decoded.has_value() || decoded->size() != 16U) {
            return std::nullopt;
        }

        IN_ADDR address{};
        const std::string address_text{local_address};
        if (InetPtonA(AF_INET, address_text.c_str(), &address) != 1) {
            return std::nullopt;
        }

        std::array<std::uint8_t, 32U> proof{};
        auto output = proof.begin();
        output = std::copy(decoded->begin(), decoded->end(), output);
        const auto* const address_bytes =
            reinterpret_cast<const std::uint8_t*>(&address.S_un.S_addr);
        output = std::copy_n(address_bytes, 4U, output);
        static_cast<void>(std::copy(device_id_.begin(), device_id_.end(), output));

        BCRYPT_PKCS1_PADDING_INFO padding{.pszAlgId = nullptr};
        ULONG signature_bytes = 0U;
        if (!NtSucceeded(BCryptSignHash(
                key_, &padding, proof.data(), static_cast<ULONG>(proof.size()), nullptr, 0U,
                &signature_bytes, BCRYPT_PAD_PKCS1)) ||
            signature_bytes == 0U) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> signature(signature_bytes);
        if (!NtSucceeded(BCryptSignHash(
                key_, &padding, proof.data(), static_cast<ULONG>(proof.size()),
                signature.data(), static_cast<ULONG>(signature.size()), &signature_bytes,
                BCRYPT_PAD_PKCS1))) {
            return std::nullopt;
        }
        signature.resize(signature_bytes);
        return EncodeBase64(signature);
    }

    [[nodiscard]] std::optional<airplaywin::crypto::RaopAesMaterial> DecryptAesMaterial(
        const std::string_view encrypted_key,
        const std::string_view initialization_vector) const {
        if (!Available() || encrypted_key.empty() || initialization_vector.empty() ||
            encrypted_key.size() > 1'024U || initialization_vector.size() > 128U) {
            return std::nullopt;
        }

        std::string padded_key{encrypted_key};
        padded_key.append((4U - (padded_key.size() % 4U)) % 4U, '=');
        std::string padded_iv{initialization_vector};
        padded_iv.append((4U - (padded_iv.size() % 4U)) % 4U, '=');
        const auto encrypted = DecodeBase64(padded_key, CRYPT_STRING_BASE64);
        const auto iv = DecodeBase64(padded_iv, CRYPT_STRING_BASE64);
        if (!encrypted.has_value() || encrypted->size() != 256U || !iv.has_value() ||
            iv->size() != 16U) {
            return std::nullopt;
        }

        BCRYPT_OAEP_PADDING_INFO padding{
            .pszAlgId = BCRYPT_SHA1_ALGORITHM,
            .pbLabel = nullptr,
            .cbLabel = 0U,
        };
        auto* const encrypted_data = const_cast<std::uint8_t*>(encrypted->data());
        ULONG clear_bytes = 0U;
        if (!NtSucceeded(BCryptDecrypt(
                key_, encrypted_data, static_cast<ULONG>(encrypted->size()), &padding,
                nullptr, 0U, nullptr, 0U, &clear_bytes, BCRYPT_PAD_OAEP)) ||
            clear_bytes != 16U) {
            return std::nullopt;
        }
        std::array<std::uint8_t, 16U> clear_key{};
        if (!NtSucceeded(BCryptDecrypt(
                key_, encrypted_data, static_cast<ULONG>(encrypted->size()), &padding,
                nullptr, 0U, clear_key.data(), static_cast<ULONG>(clear_key.size()),
                &clear_bytes, BCRYPT_PAD_OAEP)) ||
            clear_bytes != clear_key.size()) {
            SecureZeroMemory(clear_key.data(), clear_key.size());
            return std::nullopt;
        }

        airplaywin::crypto::RaopAesMaterial material;
        std::ranges::transform(clear_key, material.key.begin(),
                               [](const std::uint8_t value) {
                                   return static_cast<std::byte>(value);
                               });
        std::ranges::transform(*iv, material.iv.begin(), [](const std::uint8_t value) {
            return static_cast<std::byte>(value);
        });
        SecureZeroMemory(clear_key.data(), clear_key.size());
        return material;
    }

private:
    discovery::DeviceId device_id_{};
    BCRYPT_ALG_HANDLE algorithm_{nullptr};
    BCRYPT_KEY_HANDLE key_{nullptr};
};

WindowsRaopCryptoProvider::WindowsRaopCryptoProvider(const discovery::DeviceId device_id)
    : impl_(std::make_unique<Impl>(device_id)) {}

WindowsRaopCryptoProvider::~WindowsRaopCryptoProvider() = default;

bool WindowsRaopCryptoProvider::Available() const noexcept {
    return impl_ != nullptr && impl_->Available();
}

std::optional<std::string> WindowsRaopCryptoProvider::BuildAppleResponse(
    const std::string_view apple_challenge,
    const std::string_view local_address) const {
    return impl_ != nullptr
               ? impl_->BuildAppleResponse(apple_challenge, local_address)
               : std::nullopt;
}

std::optional<airplaywin::crypto::RaopAesMaterial>
WindowsRaopCryptoProvider::DecryptAesMaterial(
    const std::string_view encrypted_key,
    const std::string_view initialization_vector) const {
    return impl_ != nullptr
               ? impl_->DecryptAesMaterial(encrypted_key, initialization_vector)
               : std::nullopt;
}

}  // namespace airplaywin::windows::crypto
