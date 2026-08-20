#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace airplaywin::discovery {

using DeviceId = std::array<std::uint8_t, 6U>;

struct TxtProperty final {
    std::wstring key{};
    std::wstring value{};

    friend bool operator==(const TxtProperty&, const TxtProperty&) = default;
};

struct ServiceDefinition final {
    std::wstring instance_name{};
    std::wstring service_type{};
    std::wstring domain{L"local"};
    std::wstring host_name{};
    std::uint16_t port{};
    std::vector<TxtProperty> txt_properties{};
};

struct DiscoveryConfig final {
    std::wstring device_name{L"AirPlayWin"};
    DeviceId device_id{};
    std::uint16_t raop_port{5'000U};
    std::uint16_t airplay_port{7'000U};
    bool advertise_raop{true};
    bool advertise_airplay{true};
    bool include_virtual_interfaces{false};
};

struct NetworkInterfaceInfo final {
    std::uint32_t ipv4_interface_index{};
    std::wstring friendly_name{};
    std::wstring description{};
    std::wstring ipv4_address_text{};
    std::uint32_t ipv4_address{};
    DeviceId hardware_address{};
    std::size_t hardware_address_length{};
    bool is_up{};
    bool supports_multicast{};
    bool has_ipv4{};
    bool is_loopback{};
    bool is_tunnel{};
    bool is_virtual{};
};

struct DiscoveryDiagnostics final {
    bool running{};
    std::uint32_t active_interface_count{};
    std::uint32_t registered_service_count{};
    std::uint64_t publication_generation{};
    std::uint64_t network_change_count{};
    std::uint64_t name_conflict_count{};
    std::uint64_t registration_failure_count{};
    std::wstring advertised_name{};
    std::uint32_t last_error{};
};

}  // namespace airplaywin::discovery
