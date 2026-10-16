#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "platform/windows/system/WindowsFirewallManager.h"

namespace airplaywin::app {

// Shared platform commands used by both the console CLI and the GUI
// (firewall rules, endpoint calibration, device/interface enumeration).

struct FirewallCommandResult final {
    bool succeeded{false};
    std::uint32_t last_error{0U};
    std::wstring detail{};
};

[[nodiscard]] FirewallCommandResult RunFirewallCommand(
    const std::wstring& executable_path, std::uint16_t raop_port,
    std::uint16_t airplay_port, bool classic_raop, bool install, bool remove,
    bool query_status) noexcept;

struct EndpointCalibrationCommandResult final {
    bool succeeded{false};
    std::uint32_t last_error{0U};
    std::optional<std::int64_t> offset_microseconds{};
    std::wstring detail{};
};

[[nodiscard]] EndpointCalibrationCommandResult RunEndpointCalibrationCommand(
    bool save, bool show, bool clear, const std::wstring& device_id,
    std::int64_t offset_microseconds) noexcept;

struct AudioDeviceInfo final {
    std::wstring id{};
    std::wstring friendly_name{};
    bool is_default{false};
    std::uint32_t state{0U};
};

[[nodiscard]] std::vector<AudioDeviceInfo> EnumerateAudioDevices();

struct NetworkInterfaceInfo final {
    std::uint32_t ipv4_interface_index{0U};
    std::wstring ipv4_address_text{};
    std::wstring friendly_name{};
    std::wstring description{};
    bool is_virtual{false};
};

[[nodiscard]] std::vector<NetworkInterfaceInfo> EnumerateNetworkInterfaces(
    bool include_virtual_interfaces);

}  // namespace airplaywin::app
