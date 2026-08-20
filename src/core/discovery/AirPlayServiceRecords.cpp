#include "core/discovery/AirPlayServiceRecords.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <utility>

namespace airplaywin::discovery {
namespace {

constexpr std::size_t kMaxAdvertisedNameUtf8Bytes = 50U;

[[nodiscard]] int HexValue(const wchar_t value) noexcept {
    if (value >= L'0' && value <= L'9') {
        return static_cast<int>(value - L'0');
    }
    if (value >= L'a' && value <= L'f') {
        return 10 + static_cast<int>(value - L'a');
    }
    if (value >= L'A' && value <= L'F') {
        return 10 + static_cast<int>(value - L'A');
    }
    return -1;
}

[[nodiscard]] std::size_t Utf8CodePointBytes(const wchar_t value) noexcept {
    const auto code_unit = static_cast<std::uint32_t>(value);
    if (code_unit <= 0x7FU) {
        return 1U;
    }
    if (code_unit <= 0x7FFU) {
        return 2U;
    }
    if (code_unit >= 0xD800U && code_unit <= 0xDBFFU) {
        return 4U;
    }
    return 3U;
}

[[nodiscard]] std::wstring TruncateUtf8(const std::wstring_view value,
                                        const std::size_t byte_limit) {
    std::wstring result;
    result.reserve(std::min(value.size(), byte_limit));
    std::size_t bytes = 0U;
    for (std::size_t index = 0U; index < value.size(); ++index) {
        const auto code_unit = static_cast<std::uint32_t>(value[index]);
        if (code_unit >= 0xD800U && code_unit <= 0xDBFFU) {
            const bool has_low_surrogate = index + 1U < value.size() &&
                static_cast<std::uint32_t>(value[index + 1U]) >= 0xDC00U &&
                static_cast<std::uint32_t>(value[index + 1U]) <= 0xDFFFU;
            if (!has_low_surrogate) {
                if (bytes + 1U > byte_limit) {
                    break;
                }
                result.push_back(L'_');
                ++bytes;
                continue;
            }
        } else if (code_unit >= 0xDC00U && code_unit <= 0xDFFFU) {
            if (bytes + 1U > byte_limit) {
                break;
            }
            result.push_back(L'_');
            ++bytes;
            continue;
        }
        const auto character_bytes = Utf8CodePointBytes(value[index]);
        if (bytes + character_bytes > byte_limit) {
            break;
        }
        result.push_back(value[index]);
        bytes += character_bytes;
        if (code_unit >= 0xD800U && code_unit <= 0xDBFFU) {
            result.push_back(value[++index]);
        }
    }
    return result;
}

[[nodiscard]] std::size_t Utf8Size(const std::wstring_view value) noexcept {
    std::size_t bytes = 0U;
    for (std::size_t index = 0U; index < value.size(); ++index) {
        bytes += Utf8CodePointBytes(value[index]);
        const auto code_unit = static_cast<std::uint32_t>(value[index]);
        if (code_unit >= 0xD800U && code_unit <= 0xDBFFU && index + 1U < value.size()) {
            const auto low = static_cast<std::uint32_t>(value[index + 1U]);
            if (low >= 0xDC00U && low <= 0xDFFFU) {
                ++index;
            }
        }
    }
    return bytes;
}

[[nodiscard]] std::vector<TxtProperty> MakeRaopProperties() {
    return {
        {L"txtvers", L"1"}, {L"ch", L"2"},       {L"cn", L"0"},
        {L"da", L"true"},  {L"et", L"0"},       {L"md", L"0,1,2"},
        {L"pw", L"false"}, {L"sr", L"44100"},   {L"ss", L"16"},
        {L"sv", L"true"},  {L"tp", L"TCP,UDP"}, {L"vn", L"65537"},
        {L"vs", L"366.0"}, {L"am", L"AirPlayWin1,1"}, {L"sf", L"0x4"},
    };
}

[[nodiscard]] std::vector<TxtProperty> MakeAirPlayProperties(const DeviceId& device_id) {
    return {
        {L"deviceid", FormatDeviceId(device_id, true)},
        {L"features", L"0x40440200"},
        {L"flags", L"0x4"},
        {L"model", L"AirPlayWin1,1"},
        {L"manufacturer", L"AirPlayWin"},
        {L"protovers", L"1.1"},
        {L"srcvers", L"366.0"},
        {L"acl", L"0"},
        {L"rsf", L"0x0"},
        {L"pw", L"false"},
    };
}

}  // namespace

bool IsZeroDeviceId(const DeviceId& device_id) noexcept {
    return std::ranges::all_of(device_id, [](const std::uint8_t value) { return value == 0U; });
}

bool ParseDeviceId(const std::wstring_view text, DeviceId& device_id) noexcept {
    std::array<int, 12U> digits{};
    std::size_t digit_count = 0U;
    for (const wchar_t character : text) {
        const int value = HexValue(character);
        if (value >= 0) {
            if (digit_count >= digits.size()) {
                return false;
            }
            digits[digit_count++] = value;
        } else if (character != L':' && character != L'-') {
            return false;
        }
    }
    if (digit_count != digits.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < device_id.size(); ++index) {
        device_id[index] = static_cast<std::uint8_t>((digits[index * 2U] << 4) |
                                                     digits[index * 2U + 1U]);
    }
    return true;
}

std::wstring FormatDeviceId(const DeviceId& device_id, const bool with_colons) {
    constexpr std::array<wchar_t, 16U> kHex = {
        L'0', L'1', L'2', L'3', L'4', L'5', L'6', L'7',
        L'8', L'9', L'A', L'B', L'C', L'D', L'E', L'F',
    };
    std::wstring result;
    result.reserve(with_colons ? 17U : 12U);
    for (std::size_t index = 0U; index < device_id.size(); ++index) {
        if (with_colons && index != 0U) {
            result.push_back(L':');
        }
        const auto value = device_id[index];
        result.push_back(kHex[value >> 4U]);
        result.push_back(kHex[value & 0x0FU]);
    }
    return result;
}

DeviceId DeviceIdFromStableSeed(const std::wstring_view seed) noexcept {
    constexpr std::uint64_t kFnvOffset = 14'695'981'039'346'656'037ULL;
    constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;
    std::uint64_t hash = kFnvOffset;
    for (const wchar_t character : seed) {
        const auto value = static_cast<std::uint16_t>(character);
        hash ^= static_cast<std::uint8_t>(value & 0xFFU);
        hash *= kFnvPrime;
        hash ^= static_cast<std::uint8_t>(value >> 8U);
        hash *= kFnvPrime;
    }
    DeviceId result{};
    for (std::size_t index = 0U; index < result.size(); ++index) {
        const auto shift = static_cast<unsigned int>((result.size() - 1U - index) * 8U);
        result[index] = static_cast<std::uint8_t>((hash >> shift) & 0xFFU);
    }
    result[0] = static_cast<std::uint8_t>((result[0] | 0x02U) & 0xFEU);
    return result;
}

std::wstring NormalizeDeviceName(const std::wstring_view name) {
    std::wstring normalized;
    normalized.reserve(name.size());
    for (const wchar_t character : name) {
        const auto value = static_cast<std::uint32_t>(character);
        if (std::iswspace(character) != 0) {
            normalized.push_back(L' ');
        } else if (character == L'.' || character == L'\\' || value < 0x20U ||
                   value == 0x7FU) {
            normalized.push_back(L'_');
        } else {
            normalized.push_back(character);
        }
    }
    const auto first = normalized.find_first_not_of(L" \t");
    if (first == std::wstring::npos) {
        normalized = L"AirPlayWin";
    } else {
        const auto last = normalized.find_last_not_of(L" \t");
        normalized = normalized.substr(first, last - first + 1U);
    }
    normalized = TruncateUtf8(normalized, kMaxAdvertisedNameUtf8Bytes);
    return normalized.empty() ? std::wstring{L"AirPlayWin"} : normalized;
}

std::wstring MakeConflictResolvedName(const std::wstring_view base_name,
                                      const std::uint32_t ordinal) {
    const std::wstring normalized = NormalizeDeviceName(base_name);
    if (ordinal <= 1U) {
        return normalized;
    }
    const std::wstring suffix = L" (" + std::to_wstring(ordinal) + L")";
    const auto suffix_bytes = Utf8Size(suffix);
    const auto base_limit = suffix_bytes < kMaxAdvertisedNameUtf8Bytes
                                ? kMaxAdvertisedNameUtf8Bytes - suffix_bytes
                                : 0U;
    return TruncateUtf8(normalized, base_limit) + suffix;
}

std::wstring NormalizeHostLabel(const std::wstring_view host_name) {
    std::wstring result;
    result.reserve(std::min<std::size_t>(host_name.size(), 63U));
    bool previous_hyphen = false;
    for (const wchar_t character : host_name) {
        wchar_t output = L'-';
        if ((character >= L'a' && character <= L'z') ||
            (character >= L'A' && character <= L'Z') ||
            (character >= L'0' && character <= L'9')) {
            output = character;
        }
        if (output == L'-' && (result.empty() || previous_hyphen)) {
            continue;
        }
        if (result.size() == 63U) {
            break;
        }
        result.push_back(output);
        previous_hyphen = output == L'-';
    }
    while (!result.empty() && result.back() == L'-') {
        result.pop_back();
    }
    return result.empty() ? std::wstring{L"airplaywin"} : result;
}

std::wstring BuildServiceFqdn(const ServiceDefinition& service) {
    return service.instance_name + L"." + service.service_type + L"." + service.domain;
}

std::vector<ServiceDefinition> BuildAirPlayServiceRecords(
    const DiscoveryConfig& config,
    const std::wstring_view advertised_name,
    const std::wstring_view host_name) {
    std::vector<ServiceDefinition> services;
    services.reserve(static_cast<std::size_t>(config.advertise_raop) +
                     static_cast<std::size_t>(config.advertise_airplay));
    const std::wstring normalized_name = NormalizeDeviceName(advertised_name);
    const std::wstring normalized_host = NormalizeHostLabel(host_name) + L".local";

    if (config.advertise_raop) {
        services.push_back(ServiceDefinition{
            .instance_name = FormatDeviceId(config.device_id, false) + L"@" + normalized_name,
            .service_type = L"_raop._tcp",
            .domain = L"local",
            .host_name = normalized_host,
            .port = config.raop_port,
            .txt_properties = MakeRaopProperties(),
        });
    }
    if (config.advertise_airplay) {
        services.push_back(ServiceDefinition{
            .instance_name = normalized_name,
            .service_type = L"_airplay._tcp",
            .domain = L"local",
            .host_name = normalized_host,
            .port = config.airplay_port,
            .txt_properties = MakeAirPlayProperties(config.device_id),
        });
    }
    return services;
}

}  // namespace airplaywin::discovery
