#pragma once

#include "core/discovery/DiscoveryTypes.h"

namespace airplaywin::discovery {

[[nodiscard]] bool IsEligibleDiscoveryInterface(const NetworkInterfaceInfo& interface_info,
                                                bool include_virtual_interfaces) noexcept;

}  // namespace airplaywin::discovery
