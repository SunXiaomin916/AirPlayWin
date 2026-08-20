#include "TestFramework.h"

#include <algorithm>

#include "core/discovery/AirPlayServiceRecords.h"
#include "platform/windows/network/WindowsDiscoveryService.h"
#include "platform/windows/network/WindowsNetworkInterfaceEnumerator.h"

void TestWindowsDiscovery() {
    using airplaywin::windows::network::WindowsNetworkInterfaceEnumerator;

    const auto host_name = WindowsNetworkInterfaceEnumerator::LocalHostName();
    APW_EXPECT(!host_name.empty());
    const auto device_id = WindowsNetworkInterfaceEnumerator::SystemDeviceId();
    APW_EXPECT(!airplaywin::discovery::IsZeroDeviceId(device_id));

    const auto interfaces = WindowsNetworkInterfaceEnumerator::Enumerate();
    for (const auto& interface_info : interfaces) {
        APW_EXPECT(interface_info.ipv4_interface_index != 0U);
        APW_EXPECT(interface_info.is_up);
        APW_EXPECT(interface_info.supports_multicast);
        APW_EXPECT(interface_info.has_ipv4);
        APW_EXPECT(!interface_info.is_loopback);
        APW_EXPECT(!interface_info.is_tunnel);
        APW_EXPECT(!interface_info.is_virtual);
        APW_EXPECT(!interface_info.ipv4_address_text.empty());
    }
    const auto duplicate = std::adjacent_find(
        interfaces.begin(),
        interfaces.end(),
        [](const auto& left, const auto& right) {
            return left.ipv4_interface_index == right.ipv4_interface_index;
        });
    APW_EXPECT(duplicate == interfaces.end());

    airplaywin::windows::network::WindowsDiscoveryService service;
    airplaywin::discovery::DiscoveryConfig invalid_config{};
    invalid_config.advertise_raop = false;
    invalid_config.advertise_airplay = false;
    APW_EXPECT(!service.Start(invalid_config));
    APW_EXPECT(service.Diagnostics().last_error != 0U);
    service.Stop();
}
