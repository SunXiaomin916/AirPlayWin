#pragma once

#include <string>
#include <vector>

#include "core/discovery/DiscoveryTypes.h"

namespace airplaywin::windows::network {

class WindowsNetworkInterfaceEnumerator final {
public:
    [[nodiscard]] static std::vector<discovery::NetworkInterfaceInfo> Enumerate(
        bool include_virtual_interfaces = false);
    [[nodiscard]] static discovery::DeviceId SystemDeviceId();
    [[nodiscard]] static std::wstring LocalHostName();
};

}  // namespace airplaywin::windows::network
