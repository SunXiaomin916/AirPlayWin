#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace airplaywin::app {

// Persisted GUI settings. Every receiver feature exposed in the UI maps
// onto one of these fields; the schema is versioned so future fields can
// be added without breaking older settings files.
struct AppSettings final {
    std::wstring device_name{L"AirPlayWin"};
    std::wstring output_device_id{};
    bool follow_default_device{true};
    bool low_latency{false};
    bool exclusive_audio{false};
    bool strict_exclusive{false};
    std::uint16_t raop_port{5'000U};
    std::uint16_t airplay_port{7'000U};
    bool classic_raop{true};
    bool include_virtual_interfaces{false};
    bool experimental_buffered_timing{false};
    std::uint32_t buffered_timing_milliseconds{120U};
    bool experimental_ptp_timing{false};
    std::string ptp_interface_address{"0.0.0.0"};
    std::int64_t endpoint_calibration_offset_microseconds{0};
    float volume{1.0F};
    bool autostart{false};
    bool minimize_to_tray{true};
    std::uint32_t diagnostics_interval_seconds{1U};
};

class SettingsStore final {
public:
    explicit SettingsStore(std::filesystem::path path);

    [[nodiscard]] bool Load(AppSettings& settings) noexcept;
    [[nodiscard]] bool Save(const AppSettings& settings) noexcept;
    [[nodiscard]] std::wstring LastError() const noexcept;

private:
    std::filesystem::path path_{};
    std::wstring last_error_{};
};

}  // namespace airplaywin::app
