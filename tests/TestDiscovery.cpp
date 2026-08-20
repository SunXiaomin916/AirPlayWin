#include "TestFramework.h"

#include <algorithm>
#include <cstdint>
#include <string>

#include "core/discovery/AirPlayServiceRecords.h"
#include "core/discovery/NetworkInterfacePolicy.h"

namespace {

using airplaywin::discovery::BuildAirPlayServiceRecords;
using airplaywin::discovery::BuildServiceFqdn;
using airplaywin::discovery::DeviceId;
using airplaywin::discovery::DiscoveryConfig;
using airplaywin::discovery::NetworkInterfaceInfo;
using airplaywin::discovery::ServiceDefinition;
using airplaywin::discovery::TxtProperty;

[[nodiscard]] const ServiceDefinition& FindService(const std::vector<ServiceDefinition>& services,
                                                   const std::wstring& type) {
    const auto found = std::ranges::find(services, type, &ServiceDefinition::service_type);
    APW_EXPECT(found != services.end());
    return *found;
}

[[nodiscard]] bool HasProperty(const ServiceDefinition& service,
                               const std::wstring& key,
                               const std::wstring& value) {
    return std::ranges::find(service.txt_properties, TxtProperty{key, value}) !=
           service.txt_properties.end();
}

}  // namespace

void TestDiscovery() {
    DeviceId parsed{};
    APW_EXPECT(airplaywin::discovery::ParseDeviceId(L"02:11:22:AA:BB:CC", parsed));
    APW_EXPECT(parsed == (DeviceId{0x02U, 0x11U, 0x22U, 0xAAU, 0xBBU, 0xCCU}));
    APW_EXPECT(airplaywin::discovery::FormatDeviceId(parsed, true) == L"02:11:22:AA:BB:CC");
    APW_EXPECT(airplaywin::discovery::FormatDeviceId(parsed, false) == L"021122AABBCC");
    APW_EXPECT(!airplaywin::discovery::ParseDeviceId(L"02:11:22:GG:BB:CC", parsed));
    APW_EXPECT(!airplaywin::discovery::ParseDeviceId(L"02:11:22:AA:BB", parsed));

    const DeviceId generated = airplaywin::discovery::DeviceIdFromStableSeed(L"machine-guid");
    APW_EXPECT(generated == airplaywin::discovery::DeviceIdFromStableSeed(L"machine-guid"));
    APW_EXPECT(!airplaywin::discovery::IsZeroDeviceId(generated));
    APW_EXPECT((generated[0] & 0x01U) == 0U);
    APW_EXPECT((generated[0] & 0x02U) != 0U);

    APW_EXPECT(airplaywin::discovery::NormalizeDeviceName(L"  Living.Room\\Main  ") ==
               L"Living_Room_Main");
    APW_EXPECT(airplaywin::discovery::NormalizeDeviceName(L" \t ") == L"AirPlayWin");
    APW_EXPECT(airplaywin::discovery::NormalizeDeviceName(std::wstring(100U, L'x')).size() ==
               50U);
    APW_EXPECT(airplaywin::discovery::NormalizeDeviceName(std::wstring(30U, L'音')).size() ==
               16U);
    APW_EXPECT(airplaywin::discovery::MakeConflictResolvedName(L"Living Room", 1U) ==
               L"Living Room");
    APW_EXPECT(airplaywin::discovery::MakeConflictResolvedName(L"Living Room", 3U) ==
               L"Living Room (3)");
    APW_EXPECT(airplaywin::discovery::NormalizeHostLabel(L"desktop.test_name") ==
               L"desktop-test-name");

    DiscoveryConfig config{
        .device_name = L"Living Room",
        .device_id = DeviceId{0x02U, 0x11U, 0x22U, 0xAAU, 0xBBU, 0xCCU},
        .raop_port = 5'000U,
        .airplay_port = 7'000U,
        .advertise_raop = true,
        .advertise_airplay = true,
        .include_virtual_interfaces = false,
    };
    const auto services = BuildAirPlayServiceRecords(config, L"Living Room", L"desktop");
    APW_EXPECT(services.size() == 2U);

    const auto& raop = FindService(services, L"_raop._tcp");
    APW_EXPECT(raop.instance_name == L"021122AABBCC@Living Room");
    APW_EXPECT(raop.port == 5'000U);
    APW_EXPECT(raop.host_name == L"desktop.local");
    APW_EXPECT(BuildServiceFqdn(raop) == L"021122AABBCC@Living Room._raop._tcp.local");
    APW_EXPECT(HasProperty(raop, L"txtvers", L"1"));
    APW_EXPECT(HasProperty(raop, L"ch", L"2"));
    APW_EXPECT(HasProperty(raop, L"cn", L"0"));
    APW_EXPECT(HasProperty(raop, L"et", L"0"));

    const auto& airplay = FindService(services, L"_airplay._tcp");
    APW_EXPECT(airplay.instance_name == L"Living Room");
    APW_EXPECT(airplay.port == 7'000U);
    APW_EXPECT(BuildServiceFqdn(airplay) == L"Living Room._airplay._tcp.local");
    APW_EXPECT(HasProperty(airplay, L"deviceid", L"02:11:22:AA:BB:CC"));
    APW_EXPECT(HasProperty(airplay, L"features", L"0x40440200"));
    APW_EXPECT(HasProperty(airplay, L"protovers", L"1.1"));

    NetworkInterfaceInfo interface_info{
        .ipv4_interface_index = 7U,
        .is_up = true,
        .supports_multicast = true,
        .has_ipv4 = true,
    };
    APW_EXPECT(airplaywin::discovery::IsEligibleDiscoveryInterface(interface_info, false));
    interface_info.is_virtual = true;
    APW_EXPECT(!airplaywin::discovery::IsEligibleDiscoveryInterface(interface_info, false));
    APW_EXPECT(airplaywin::discovery::IsEligibleDiscoveryInterface(interface_info, true));
    interface_info.is_loopback = true;
    APW_EXPECT(!airplaywin::discovery::IsEligibleDiscoveryInterface(interface_info, true));
    interface_info.is_loopback = false;
    interface_info.ipv4_interface_index = 0U;
    APW_EXPECT(!airplaywin::discovery::IsEligibleDiscoveryInterface(interface_info, true));
}
