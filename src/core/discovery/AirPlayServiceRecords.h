#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/discovery/DiscoveryTypes.h"

namespace airplaywin::discovery {

[[nodiscard]] bool IsZeroDeviceId(const DeviceId& device_id) noexcept;
[[nodiscard]] bool ParseDeviceId(std::wstring_view text, DeviceId& device_id) noexcept;
[[nodiscard]] std::wstring FormatDeviceId(const DeviceId& device_id, bool with_colons);
[[nodiscard]] DeviceId DeviceIdFromStableSeed(std::wstring_view seed) noexcept;

[[nodiscard]] std::wstring NormalizeDeviceName(std::wstring_view name);
[[nodiscard]] std::wstring MakeConflictResolvedName(std::wstring_view base_name,
                                                    std::uint32_t ordinal);
[[nodiscard]] std::wstring NormalizeHostLabel(std::wstring_view host_name);
[[nodiscard]] std::wstring BuildServiceFqdn(const ServiceDefinition& service);

[[nodiscard]] std::vector<ServiceDefinition> BuildAirPlayServiceRecords(
    const DiscoveryConfig& config,
    std::wstring_view advertised_name,
    std::wstring_view host_name);

}  // namespace airplaywin::discovery
