#include "platform/windows/audio/WindowsEndpointCalibrationStore.h"

#include <Windows.h>

#include <cerrno>
#include <climits>
#include <cwchar>
#include <limits>
#include <utility>
#include <vector>

namespace airplaywin::windows::audio {
namespace {

class RegistryKey final {
public:
    explicit RegistryKey(HKEY key = nullptr) noexcept : key_(key) {}
    ~RegistryKey() {
        if (key_ != nullptr) {
            static_cast<void>(RegCloseKey(key_));
        }
    }

    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;

    [[nodiscard]] HKEY Get() const noexcept { return key_; }

private:
    HKEY key_{nullptr};
};

}  // namespace

WindowsEndpointCalibrationStore::WindowsEndpointCalibrationStore(
    std::wstring registry_path)
    : registry_path_(std::move(registry_path)) {}

bool WindowsEndpointCalibrationStore::Save(
    const airplaywin::group::EndpointCalibration& calibration) {
    if (!calibration.IsValid()) {
        last_error_.store(ERROR_INVALID_PARAMETER, std::memory_order_release);
        return false;
    }
    const auto endpoint = ToWide(calibration.endpoint_id);
    if (!endpoint.has_value()) {
        last_error_.store(ERROR_NO_UNICODE_TRANSLATION, std::memory_order_release);
        return false;
    }
    HKEY raw_key = nullptr;
    const auto created = RegCreateKeyExW(HKEY_CURRENT_USER, registry_path_.c_str(), 0,
                                         nullptr, 0, KEY_SET_VALUE, nullptr,
                                         &raw_key, nullptr);
    if (created != ERROR_SUCCESS) {
        last_error_.store(created, std::memory_order_release);
        return false;
    }
    RegistryKey key{raw_key};
    const auto payload = std::to_wstring(calibration.offset_microseconds) + L"|" +
                         std::to_wstring(static_cast<unsigned>(calibration.source)) +
                         L"|" + *endpoint;
    const auto value_name = ValueName(calibration.endpoint_id);
    const auto bytes = static_cast<DWORD>((payload.size() + 1U) * sizeof(wchar_t));
    const auto result = RegSetValueExW(
        key.Get(), value_name.c_str(), 0, REG_SZ,
        reinterpret_cast<const BYTE*>(payload.c_str()), bytes);
    last_error_.store(result, std::memory_order_release);
    return result == ERROR_SUCCESS;
}

std::optional<airplaywin::group::EndpointCalibration>
WindowsEndpointCalibrationStore::Load(const std::string_view endpoint_id) const {
    if (endpoint_id.empty() || endpoint_id.size() > 2'048U) {
        last_error_.store(ERROR_INVALID_PARAMETER, std::memory_order_release);
        return std::nullopt;
    }
    HKEY raw_key = nullptr;
    const auto opened = RegOpenKeyExW(HKEY_CURRENT_USER, registry_path_.c_str(), 0,
                                      KEY_QUERY_VALUE, &raw_key);
    if (opened != ERROR_SUCCESS) {
        last_error_.store(opened, std::memory_order_release);
        return std::nullopt;
    }
    RegistryKey key{raw_key};
    const auto value_name = ValueName(endpoint_id);
    DWORD type = 0U;
    DWORD bytes = 0U;
    auto result = RegQueryValueExW(key.Get(), value_name.c_str(), nullptr, &type,
                                   nullptr, &bytes);
    if (result != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t) ||
        bytes > 64U * 1'024U || bytes % sizeof(wchar_t) != 0U) {
        last_error_.store(result != ERROR_SUCCESS ? result : ERROR_INVALID_DATA,
                          std::memory_order_release);
        return std::nullopt;
    }
    std::vector<wchar_t> payload(bytes / sizeof(wchar_t));
    result = RegQueryValueExW(key.Get(), value_name.c_str(), nullptr, &type,
                              reinterpret_cast<BYTE*>(payload.data()), &bytes);
    if (result != ERROR_SUCCESS || payload.empty()) {
        last_error_.store(result != ERROR_SUCCESS ? result : ERROR_INVALID_DATA,
                          std::memory_order_release);
        return std::nullopt;
    }
    payload.back() = L'\0';
    const std::wstring_view text{payload.data()};
    const auto first = text.find(L'|');
    const auto second = first == std::wstring_view::npos
                            ? std::wstring_view::npos
                            : text.find(L'|', first + 1U);
    if (first == std::wstring_view::npos || second == std::wstring_view::npos) {
        last_error_.store(ERROR_INVALID_DATA, std::memory_order_release);
        return std::nullopt;
    }
    const std::wstring offset_text{text.substr(0U, first)};
    errno = 0;
    wchar_t* end = nullptr;
    const auto offset = std::wcstoll(offset_text.c_str(), &end, 10);
    const auto source_text = text.substr(first + 1U, second - first - 1U);
    if (errno == ERANGE || end == offset_text.c_str() || *end != L'\0' ||
        (source_text != L"0" && source_text != L"1")) {
        last_error_.store(ERROR_INVALID_DATA, std::memory_order_release);
        return std::nullopt;
    }
    const auto stored_id = ToUtf8(text.substr(second + 1U));
    if (!stored_id.has_value() || *stored_id != endpoint_id) {
        last_error_.store(ERROR_INVALID_DATA, std::memory_order_release);
        return std::nullopt;
    }
    airplaywin::group::EndpointCalibration calibration{
        .endpoint_id = *stored_id,
        .offset_microseconds = offset,
        .source = source_text == L"1"
                      ? airplaywin::group::EndpointCalibrationSource::Measured
                      : airplaywin::group::EndpointCalibrationSource::Manual,
    };
    if (!calibration.IsValid()) {
        last_error_.store(ERROR_INVALID_DATA, std::memory_order_release);
        return std::nullopt;
    }
    last_error_.store(ERROR_SUCCESS, std::memory_order_release);
    return calibration;
}

bool WindowsEndpointCalibrationStore::Remove(const std::string_view endpoint_id) {
    if (endpoint_id.empty() || endpoint_id.size() > 2'048U) {
        last_error_.store(ERROR_INVALID_PARAMETER, std::memory_order_release);
        return false;
    }
    HKEY raw_key = nullptr;
    const auto opened = RegOpenKeyExW(HKEY_CURRENT_USER, registry_path_.c_str(), 0,
                                      KEY_SET_VALUE, &raw_key);
    if (opened != ERROR_SUCCESS) {
        last_error_.store(opened, std::memory_order_release);
        return false;
    }
    RegistryKey key{raw_key};
    const auto result = RegDeleteValueW(key.Get(), ValueName(endpoint_id).c_str());
    last_error_.store(result, std::memory_order_release);
    return result == ERROR_SUCCESS;
}

bool WindowsEndpointCalibrationStore::SaveWindowsEndpoint(
    const std::wstring_view endpoint_id, const std::int64_t offset_microseconds,
    const airplaywin::group::EndpointCalibrationSource source) {
    const auto utf8 = ToUtf8(endpoint_id);
    if (!utf8.has_value()) {
        last_error_.store(ERROR_NO_UNICODE_TRANSLATION, std::memory_order_release);
        return false;
    }
    return Save({.endpoint_id = *utf8,
                 .offset_microseconds = offset_microseconds,
                 .source = source});
}

std::optional<airplaywin::group::EndpointCalibration>
WindowsEndpointCalibrationStore::LoadWindowsEndpoint(
    const std::wstring_view endpoint_id) const {
    const auto utf8 = ToUtf8(endpoint_id);
    if (!utf8.has_value()) {
        last_error_.store(ERROR_NO_UNICODE_TRANSLATION, std::memory_order_release);
        return std::nullopt;
    }
    return Load(*utf8);
}

bool WindowsEndpointCalibrationStore::RemoveWindowsEndpoint(
    const std::wstring_view endpoint_id) {
    const auto utf8 = ToUtf8(endpoint_id);
    if (!utf8.has_value()) {
        last_error_.store(ERROR_NO_UNICODE_TRANSLATION, std::memory_order_release);
        return false;
    }
    return Remove(*utf8);
}

std::uint32_t WindowsEndpointCalibrationStore::LastError() const noexcept {
    return last_error_.load(std::memory_order_acquire);
}

std::optional<std::string> WindowsEndpointCalibrationStore::ToUtf8(
    const std::wstring_view value) {
    if (value.empty() || value.size() > static_cast<std::size_t>(INT_MAX)) {
        return std::nullopt;
    }
    const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                            static_cast<int>(value.size()), nullptr, 0,
                                            nullptr, nullptr);
    if (length <= 0) {
        return std::nullopt;
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()), result.data(), length,
                               nullptr, nullptr) == length
               ? std::optional<std::string>{std::move(result)}
               : std::nullopt;
}

std::wstring WindowsEndpointCalibrationStore::ValueName(
    const std::string_view endpoint_id) {
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (const auto value : endpoint_id) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= 1'099'511'628'211ULL;
    }
    wchar_t name[18]{};
    static_cast<void>(swprintf_s(name, L"E%016llX",
                                 static_cast<unsigned long long>(hash)));
    return name;
}

std::optional<std::wstring> WindowsEndpointCalibrationStore::ToWide(
    const std::string_view value) {
    if (value.empty() || value.size() > static_cast<std::size_t>(INT_MAX)) {
        return std::nullopt;
    }
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                            static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return std::nullopt;
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()), result.data(), length) ==
                   length
               ? std::optional<std::wstring>{std::move(result)}
               : std::nullopt;
}

}  // namespace airplaywin::windows::audio
