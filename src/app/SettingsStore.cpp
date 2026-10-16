#include "app/SettingsStore.h"

#include <Windows.h>

#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string_view>

namespace airplaywin::app {

namespace {

// Minimal JSON object reader/writer covering exactly the AppSettings
// schema (flat object; string / number / boolean values). Written by hand
// so the project keeps its zero-dependency policy.

class JsonParser final {
public:
    explicit JsonParser(const std::string_view text) : text_(text) {}

    // Parses the flat object and invokes the callback for every key with
    // its raw value. Returns false on malformed input.
    template <typename Callback>
    bool ParseObject(Callback&& callback) const {
        std::size_t cursor = 0U;
        SkipWhitespace(cursor);
        if (cursor >= text_.size() || text_[cursor] != '{') {
            return false;
        }
        ++cursor;
        SkipWhitespace(cursor);
        if (cursor < text_.size() && text_[cursor] == '}') {
            return true;
        }
        while (cursor < text_.size()) {
            const auto key = ParseString(cursor);
            if (!key.has_value()) {
                return false;
            }
            SkipWhitespace(cursor);
            if (cursor >= text_.size() || text_[cursor] != ':') {
                return false;
            }
            ++cursor;
            SkipWhitespace(cursor);
            const auto value = ParseValue(cursor);
            if (!value.has_value()) {
                return false;
            }
            callback(*key, *value);
            SkipWhitespace(cursor);
            if (cursor >= text_.size()) {
                return false;
            }
            if (text_[cursor] == ',') {
                ++cursor;
                SkipWhitespace(cursor);
                continue;
            }
            return text_[cursor] == '}';
        }
        return false;
    }

private:
    void SkipWhitespace(std::size_t& cursor) const noexcept {
        while (cursor < text_.size() &&
               (text_[cursor] == ' ' || text_[cursor] == '\t' ||
                text_[cursor] == '\r' || text_[cursor] == '\n')) {
            ++cursor;
        }
    }

    std::optional<std::string> ParseString(std::size_t& cursor) const {
        if (cursor >= text_.size() || text_[cursor] != '"') {
            return std::nullopt;
        }
        ++cursor;
        std::string result;
        while (cursor < text_.size()) {
            const char character = text_[cursor];
            if (character == '"') {
                ++cursor;
                return result;
            }
            if (character == '\\') {
                ++cursor;
                if (cursor >= text_.size()) {
                    return std::nullopt;
                }
                switch (text_[cursor]) {
                case '"':
                    result.push_back('"');
                    break;
                case '\\':
                    result.push_back('\\');
                    break;
                case 'n':
                    result.push_back('\n');
                    break;
                case 't':
                    result.push_back('\t');
                    break;
                case 'r':
                    result.push_back('\r');
                    break;
                case 'u': {
                    if (cursor + 4U >= text_.size()) {
                        return std::nullopt;
                    }
                    unsigned code = 0U;
                    for (std::size_t index = 1U; index <= 4U; ++index) {
                        const char digit = text_[cursor + index];
                        code <<= 4U;
                        if (digit >= '0' && digit <= '9') {
                            code |= static_cast<unsigned>(digit - '0');
                        } else if (digit >= 'a' && digit <= 'f') {
                            code |= static_cast<unsigned>(digit - 'a' + 10);
                        } else if (digit >= 'A' && digit <= 'F') {
                            code |= static_cast<unsigned>(digit - 'A' + 10);
                        } else {
                            return std::nullopt;
                        }
                    }
                    cursor += 4U;
                    // Encode as UTF-8 (covers BMP code points).
                    if (code < 0x80U) {
                        result.push_back(static_cast<char>(code));
                    } else if (code < 0x800U) {
                        result.push_back(static_cast<char>(0xC0U | (code >> 6U)));
                        result.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
                    } else {
                        result.push_back(static_cast<char>(0xE0U | (code >> 12U)));
                        result.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
                        result.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
                    }
                    break;
                }
                default:
                    return std::nullopt;
                }
                ++cursor;
                continue;
            }
            result.push_back(character);
            ++cursor;
        }
        return std::nullopt;
    }

    std::optional<std::string> ParseValue(std::size_t& cursor) const {
        if (cursor >= text_.size()) {
            return std::nullopt;
        }
        const char first = text_[cursor];
        if (first == '"') {
            const auto value = ParseString(cursor);
            return value.has_value() ? std::optional<std::string>{"\"" + *value + "\""}
                                     : std::nullopt;
        }
        const auto begin = cursor;
        while (cursor < text_.size()) {
            const char character = text_[cursor];
            if (character == ',' || character == '}' ||
                character == ' ' || character == '\t' ||
                character == '\r' || character == '\n') {
                break;
            }
            ++cursor;
        }
        if (cursor == begin) {
            return std::nullopt;
        }
        return std::string{text_.substr(begin, cursor - begin)};
    }

    std::string_view text_{};
};

[[nodiscard]] std::optional<bool> ParseBool(const std::string_view text) noexcept {
    if (text == "true") {
        return true;
    }
    if (text == "false") {
        return false;
    }
    return std::nullopt;
}

template <typename Integer>
[[nodiscard]] std::optional<Integer> ParseInteger(const std::string_view text) noexcept {
    Integer value{};
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<double> ParseDouble(const std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    std::string storage{text};
    char* end = nullptr;
    const double value = std::strtod(storage.c_str(), &end);
    if (end != storage.c_str() + storage.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

void AppendEscaped(std::string& output, const std::string_view value) {
    output.push_back('"');
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (byte < 0x20U) {
                constexpr char hex[] = "0123456789abcdef";
                output += "\\u00";
                output.push_back(hex[byte >> 4U]);
                output.push_back(hex[byte & 0x0FU]);
            } else {
                output.push_back(character);
            }
            break;
        }
    }
    output.push_back('"');
}

[[nodiscard]] std::string WideToUtf8(const std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0,
                                             nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    static_cast<void>(WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()), result.data(),
                                          required, nullptr, nullptr));
    return result;
}

[[nodiscard]] std::wstring Utf8ToWide(const std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int required =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    static_cast<void>(MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()), result.data(),
                                          required));
    return result;
}

}  // namespace

SettingsStore::SettingsStore(std::filesystem::path path) : path_(std::move(path)) {}

bool SettingsStore::Load(AppSettings& settings) noexcept {
    last_error_.clear();
    std::ifstream stream{path_, std::ios::binary};
    if (!stream) {
        // A missing settings file is the normal first-run state.
        return true;
    }
    std::string text{std::istreambuf_iterator<char>{stream},
                     std::istreambuf_iterator<char>{}};
    JsonParser parser{text};
    const auto apply = [&](const std::string_view key, const std::string_view raw) {
        auto apply_bool = [&](bool& field) {
            const auto value = ParseBool(raw);
            if (value.has_value()) {
                field = *value;
            }
        };
        auto apply_u32 = [&](std::uint32_t& field) {
            const auto value = ParseInteger<std::uint32_t>(raw);
            if (value.has_value()) {
                field = *value;
            }
        };
        auto apply_i64 = [&](std::int64_t& field) {
            const auto value = ParseInteger<std::int64_t>(raw);
            if (value.has_value()) {
                field = *value;
            }
        };
        auto apply_f32 = [&](float& field) {
            const auto value = ParseDouble(raw);
            if (value.has_value()) {
                field = static_cast<float>(*value);
            }
        };
        auto apply_string = [&](std::wstring& field) {
            if (!raw.empty() && raw.front() == '"' && raw.back() == '"') {
                field = Utf8ToWide(raw.substr(1U, raw.size() - 2U));
            }
        };
        auto apply_ascii = [&](std::string& field) {
            if (!raw.empty() && raw.front() == '"' && raw.back() == '"') {
                field = raw.substr(1U, raw.size() - 2U);
            }
        };
        if (key == "deviceName") {
            apply_string(settings.device_name);
        } else if (key == "outputDeviceId") {
            apply_string(settings.output_device_id);
        } else if (key == "followDefaultDevice") {
            apply_bool(settings.follow_default_device);
        } else if (key == "lowLatency") {
            apply_bool(settings.low_latency);
        } else if (key == "exclusiveAudio") {
            apply_bool(settings.exclusive_audio);
        } else if (key == "strictExclusive") {
            apply_bool(settings.strict_exclusive);
        } else if (key == "raopPort") {
            const auto value = ParseInteger<std::uint32_t>(raw);
            if (value.has_value() && *value > 0U && *value <= 65'535U) {
                settings.raop_port = static_cast<std::uint16_t>(*value);
            }
        } else if (key == "airplayPort") {
            const auto value = ParseInteger<std::uint32_t>(raw);
            if (value.has_value() && *value > 0U && *value <= 65'535U) {
                settings.airplay_port = static_cast<std::uint16_t>(*value);
            }
        } else if (key == "classicRaop") {
            apply_bool(settings.classic_raop);
        } else if (key == "includeVirtualInterfaces") {
            apply_bool(settings.include_virtual_interfaces);
        } else if (key == "experimentalBufferedTiming") {
            apply_bool(settings.experimental_buffered_timing);
        } else if (key == "bufferedTimingMilliseconds") {
            apply_u32(settings.buffered_timing_milliseconds);
        } else if (key == "experimentalPtpTiming") {
            apply_bool(settings.experimental_ptp_timing);
        } else if (key == "ptpInterfaceAddress") {
            apply_ascii(settings.ptp_interface_address);
        } else if (key == "endpointCalibrationOffsetMicroseconds") {
            apply_i64(settings.endpoint_calibration_offset_microseconds);
        } else if (key == "volume") {
            apply_f32(settings.volume);
        } else if (key == "autostart") {
            apply_bool(settings.autostart);
        } else if (key == "minimizeToTray") {
            apply_bool(settings.minimize_to_tray);
        } else if (key == "diagnosticsIntervalSeconds") {
            apply_u32(settings.diagnostics_interval_seconds);
        }
    };
    const bool parsed = parser.ParseObject(apply);
    if (!parsed) {
        last_error_ = L"Settings file is malformed; defaults will be used.";
        return true;  // Malformed settings fall back to defaults.
    }
    return true;
}

bool SettingsStore::Save(const AppSettings& settings) noexcept {
    last_error_.clear();
    std::string json;
    json.reserve(1'024U);
    json += "{\n";
    const auto append = [&](const std::string_view key, const std::string& value,
                            const bool last) {
        json += "  \"";
        json += key;
        json += "\": ";
        json += value;
        json += last ? "\n" : ",\n";
    };
    const auto append_string = [&](const std::string_view key,
                                   const std::string_view value, const bool last) {
        std::string escaped;
        AppendEscaped(escaped, value);
        append(key, escaped, last);
    };
    append_string("deviceName", WideToUtf8(settings.device_name), false);
    append_string("outputDeviceId", WideToUtf8(settings.output_device_id), false);
    append("followDefaultDevice", settings.follow_default_device ? "true" : "false", false);
    append("lowLatency", settings.low_latency ? "true" : "false", false);
    append("exclusiveAudio", settings.exclusive_audio ? "true" : "false", false);
    append("strictExclusive", settings.strict_exclusive ? "true" : "false", false);
    append("raopPort", std::to_string(settings.raop_port), false);
    append("airplayPort", std::to_string(settings.airplay_port), false);
    append("classicRaop", settings.classic_raop ? "true" : "false", false);
    append("includeVirtualInterfaces", settings.include_virtual_interfaces ? "true" : "false",
           false);
    append("experimentalBufferedTiming",
           settings.experimental_buffered_timing ? "true" : "false", false);
    append("bufferedTimingMilliseconds",
           std::to_string(settings.buffered_timing_milliseconds), false);
    append("experimentalPtpTiming", settings.experimental_ptp_timing ? "true" : "false",
           false);
    append_string("ptpInterfaceAddress", settings.ptp_interface_address, false);
    append("endpointCalibrationOffsetMicroseconds",
           std::to_string(settings.endpoint_calibration_offset_microseconds), false);
    append("volume", std::to_string(settings.volume), false);
    append("autostart", settings.autostart ? "true" : "false", false);
    append("minimizeToTray", settings.minimize_to_tray ? "true" : "false", false);
    append("diagnosticsIntervalSeconds",
           std::to_string(settings.diagnostics_interval_seconds), true);
    json += "}\n";
    std::ofstream stream{path_, std::ios::binary | std::ios::trunc};
    if (!stream) {
        last_error_ = L"Unable to open the settings file for writing.";
        return false;
    }
    stream.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (!stream) {
        last_error_ = L"Unable to write the settings file.";
        return false;
    }
    return true;
}

std::wstring SettingsStore::LastError() const noexcept {
    return last_error_;
}

}  // namespace airplaywin::app
