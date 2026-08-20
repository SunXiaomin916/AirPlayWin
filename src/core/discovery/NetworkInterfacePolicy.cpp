#include "core/discovery/NetworkInterfacePolicy.h"

namespace airplaywin::discovery {

bool IsEligibleDiscoveryInterface(const NetworkInterfaceInfo& interface_info,
                                  const bool include_virtual_interfaces) noexcept {
    if (!interface_info.is_up || !interface_info.supports_multicast ||
        !interface_info.has_ipv4 || interface_info.ipv4_interface_index == 0U ||
        interface_info.is_loopback || interface_info.is_tunnel) {
        return false;
    }
    return include_virtual_interfaces || !interface_info.is_virtual;
}

}  // namespace airplaywin::discovery
