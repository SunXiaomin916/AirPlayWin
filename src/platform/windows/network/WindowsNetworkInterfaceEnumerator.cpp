#include <WinSock2.h>
#include <Windows.h>
#include <Iphlpapi.h>
#include <Ws2tcpip.h>

#include "platform/windows/network/WindowsNetworkInterfaceEnumerator.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwctype>
#include <string_view>
#include <vector>

#include "core/discovery/AirPlayServiceRecords.h"
#include "core/discovery/NetworkInterfacePolicy.h"

namespace airplaywin::windows::network {
namespace {

[[nodiscard]] std::wstring ToLower(std::wstring_view value) {
    std::wstring result{value};
    std::ranges::transform(result, result.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return result;
}

[[nodiscard]] bool LooksVirtual(const IP_ADAPTER_ADDRESSES& adapter) {
    const std::wstring name = adapter.FriendlyName == nullptr ? L"" : adapter.FriendlyName;
    const std::wstring description =
        adapter.Description == nullptr ? L"" : adapter.Description;
    const std::wstring searchable = ToLower(name + L" " + description);
    constexpr std::array<std::wstring_view, 13U> kVirtualMarkers = {
        L"virtual", L"hyper-v", L"vmware", L"virtualbox", L"wsl", L"docker",
        L"vpn", L"tap", L"tun", L"loopback", L"npcap", L"bluetooth", L"teredo",
    };
    return std::ranges::any_of(kVirtualMarkers, [&searchable](const std::wstring_view marker) {
        return searchable.find(marker) != std::wstring::npos;
    });
}

[[nodiscard]] const SOCKADDR_IN* FindIpv4Address(const IP_ADAPTER_ADDRESSES& adapter) noexcept {
    for (auto* address = adapter.FirstUnicastAddress; address != nullptr; address = address->Next) {
        if (address->Address.lpSockaddr != nullptr &&
            address->Address.lpSockaddr->sa_family == AF_INET) {
            const auto* ipv4 = reinterpret_cast<const SOCKADDR_IN*>(address->Address.lpSockaddr);
            if (ipv4->sin_addr.S_un.S_addr != INADDR_ANY) {
                return ipv4;
            }
        }
    }
    return nullptr;
}

[[nodiscard]] std::wstring FormatIpv4(const IN_ADDR& address) {
    std::array<wchar_t, INET_ADDRSTRLEN> buffer{};
    return InetNtopW(AF_INET, const_cast<IN_ADDR*>(&address), buffer.data(),
                     static_cast<DWORD>(buffer.size())) == nullptr
               ? std::wstring{}
               : std::wstring{buffer.data()};
}

[[nodiscard]] std::wstring ReadMachineGuid() {
    std::array<wchar_t, 128U> value{};
    DWORD byte_count = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE,
                                        L"SOFTWARE\\Microsoft\\Cryptography",
                                        L"MachineGuid",
                                        RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                                        nullptr,
                                        value.data(),
                                        &byte_count);
    if (status != ERROR_SUCCESS || value[0] == L'\0') {
        return {};
    }
    return std::wstring{value.data()};
}

}  // namespace

std::vector<discovery::NetworkInterfaceInfo> WindowsNetworkInterfaceEnumerator::Enumerate(
    const bool include_virtual_interfaces) {
    constexpr ULONG kFlags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                             GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_PREFIX;
    ULONG required_size = 16U * 1'024U;
    std::vector<std::byte> storage(required_size);
    ULONG status = ERROR_BUFFER_OVERFLOW;
    for (std::uint32_t attempt = 0U; attempt < 3U && status == ERROR_BUFFER_OVERFLOW; ++attempt) {
        auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        status = GetAdaptersAddresses(AF_UNSPEC, kFlags, nullptr, addresses, &required_size);
        if (status == ERROR_BUFFER_OVERFLOW) {
            storage.resize(required_size);
        }
    }
    if (status != NO_ERROR) {
        return {};
    }

    std::vector<discovery::NetworkInterfaceInfo> interfaces;
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
         adapter != nullptr;
         adapter = adapter->Next) {
        const SOCKADDR_IN* ipv4 = FindIpv4Address(*adapter);
        discovery::NetworkInterfaceInfo info{
            .ipv4_interface_index = adapter->IfIndex,
            .friendly_name = adapter->FriendlyName == nullptr ? L"" : adapter->FriendlyName,
            .description = adapter->Description == nullptr ? L"" : adapter->Description,
            .ipv4_address_text = ipv4 == nullptr ? L"" : FormatIpv4(ipv4->sin_addr),
            .ipv4_address = ipv4 == nullptr ? 0U : ipv4->sin_addr.S_un.S_addr,
            .hardware_address = {},
            .hardware_address_length =
                std::min<std::size_t>(adapter->PhysicalAddressLength, 6U),
            .is_up = adapter->OperStatus == IfOperStatusUp,
            .supports_multicast = (adapter->Flags & IP_ADAPTER_NO_MULTICAST) == 0U,
            .has_ipv4 = ipv4 != nullptr,
            .is_loopback = adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK,
            .is_tunnel = adapter->IfType == IF_TYPE_TUNNEL,
            .is_virtual = LooksVirtual(*adapter),
        };
        std::copy_n(adapter->PhysicalAddress,
                    info.hardware_address_length,
                    info.hardware_address.begin());
        if (discovery::IsEligibleDiscoveryInterface(info, include_virtual_interfaces)) {
            interfaces.push_back(std::move(info));
        }
    }
    std::ranges::sort(interfaces, {}, &discovery::NetworkInterfaceInfo::ipv4_interface_index);
    interfaces.erase(std::ranges::unique(interfaces, {},
                                         &discovery::NetworkInterfaceInfo::ipv4_interface_index)
                         .begin(),
                     interfaces.end());
    return interfaces;
}

discovery::DeviceId WindowsNetworkInterfaceEnumerator::SystemDeviceId() {
    std::wstring seed = ReadMachineGuid();
    if (seed.empty()) {
        seed = LocalHostName();
    }
    if (seed.empty()) {
        seed = L"AirPlayWin-Windows-Device";
    }
    return discovery::DeviceIdFromStableSeed(seed);
}

std::wstring WindowsNetworkInterfaceEnumerator::LocalHostName() {
    std::array<wchar_t, 256U> buffer{};
    DWORD length = static_cast<DWORD>(buffer.size());
    if (GetComputerNameExW(ComputerNameDnsHostname, buffer.data(), &length) == FALSE ||
        length == 0U) {
        length = static_cast<DWORD>(buffer.size());
        if (GetComputerNameW(buffer.data(), &length) == FALSE || length == 0U) {
            return L"airplaywin";
        }
    }
    return discovery::NormalizeHostLabel(std::wstring_view{buffer.data(), length});
}

}  // namespace airplaywin::windows::network
