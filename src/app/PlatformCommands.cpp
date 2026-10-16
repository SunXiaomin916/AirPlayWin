#include "app/PlatformCommands.h"

#include <Windows.h>

#include "core/group/GroupTypes.h"
#include "platform/windows/audio/WindowsAudioDeviceEnumerator.h"
#include "platform/windows/audio/WindowsEndpointCalibrationStore.h"
#include "platform/windows/network/WindowsNetworkInterfaceEnumerator.h"

namespace airplaywin::app {

using airplaywin::windows::audio::WindowsAudioDeviceEnumerator;
using airplaywin::windows::audio::WindowsEndpointCalibrationStore;
using airplaywin::windows::network::WindowsNetworkInterfaceEnumerator;
using airplaywin::windows::system::FirewallRuleHealth;
using airplaywin::windows::system::WindowsFirewallManager;

FirewallCommandResult RunFirewallCommand(
    const std::wstring& executable_path, const std::uint16_t raop_port,
    const std::uint16_t airplay_port, const bool classic_raop,
    const bool install, const bool remove, const bool query_status) noexcept {
    FirewallCommandResult result;
    const auto rules = WindowsFirewallManager::CoreRuleSpecs(
        executable_path, raop_port, airplay_port, !classic_raop);
    bool succeeded = true;
    std::wstring detail;
    for (const auto& rule : rules) {
        std::uint32_t error = ERROR_SUCCESS;
        if (remove) {
            const bool removed = WindowsFirewallManager::Remove(rule.name, error);
            succeeded = succeeded && removed;
            detail += L"Rule \"" + rule.name + L"\": " +
                      (removed ? L"removed/not present" : L"remove failed") +
                      L", error=0x" + std::to_wstring(error) + L"\n";
        } else if (install) {
            const bool installed = WindowsFirewallManager::Install(rule, error);
            succeeded = succeeded && installed;
            detail += L"Rule \"" + rule.name + L"\": " +
                      (installed ? L"installed" : L"install failed") +
                      L", error=0x" + std::to_wstring(error) + L"\n";
        } else {
            const auto status = WindowsFirewallManager::Query(rule);
            const bool healthy = status.health == FirewallRuleHealth::Healthy;
            succeeded = succeeded && healthy;
            detail += L"Rule \"" + rule.name + L"\": " +
                      [&]() -> std::wstring {
                          switch (status.health) {
                          case FirewallRuleHealth::Missing:
                              return L"missing";
                          case FirewallRuleHealth::Healthy:
                              return L"healthy";
                          case FirewallRuleHealth::Misconfigured:
                              return L"misconfigured";
                          case FirewallRuleHealth::Error:
                              return L"error";
                          }
                          return L"unknown";
                      }() +
                      (status.health == FirewallRuleHealth::Healthy ||
                               status.health == FirewallRuleHealth::Misconfigured
                           ? L", private_only=" +
                                 std::wstring(status.private_profile_only ? L"yes" : L"no") +
                                 L", program=" +
                                 std::wstring(status.executable_matches ? L"match" : L"mismatch")
                           : std::wstring{}) +
                      L", error=0x" + std::to_wstring(status.last_error) + L"\n";
            if (!query_status) {
                break;
            }
        }
        if (!query_status && !install && !remove) {
            break;
        }
    }
    result.succeeded = succeeded;
    result.detail = std::move(detail);
    return result;
}

EndpointCalibrationCommandResult RunEndpointCalibrationCommand(
    const bool save, const bool show, const bool clear,
    const std::wstring& device_id,
    const std::int64_t offset_microseconds) noexcept {
    static_cast<void>(show);  // The query branch below covers show semantics.
    EndpointCalibrationCommandResult result;
    WindowsEndpointCalibrationStore store;
    if (save) {
        const bool saved = store.SaveWindowsEndpoint(
            device_id, offset_microseconds,
            airplaywin::group::EndpointCalibrationSource::Manual);
        result.succeeded = saved;
        result.last_error = store.LastError();
        result.detail = saved ? L"Saved endpoint latency offset."
                              : L"Unable to save endpoint calibration.";
        return result;
    }
    if (clear) {
        const bool removed = store.RemoveWindowsEndpoint(device_id);
        result.succeeded = removed;
        result.last_error = store.LastError();
        result.detail = removed ? L"Cleared endpoint latency offset."
                                : L"Unable to clear endpoint calibration.";
        return result;
    }
    const auto calibration = store.LoadWindowsEndpoint(device_id);
    if (calibration.has_value()) {
        result.succeeded = true;
        result.offset_microseconds = calibration->offset_microseconds;
        result.detail = L"Loaded endpoint latency offset.";
    } else {
        result.succeeded = false;
        result.last_error = store.LastError();
        result.detail = L"No endpoint calibration is available.";
    }
    return result;
}

std::vector<AudioDeviceInfo> EnumerateAudioDevices() {
    std::vector<AudioDeviceInfo> devices;
    for (const auto& device : WindowsAudioDeviceEnumerator::EnumerateRenderDevices()) {
        devices.push_back({
            .id = device.id,
            .friendly_name = device.friendly_name,
            .is_default = device.is_default,
            .state = device.state,
        });
    }
    return devices;
}

std::vector<NetworkInterfaceInfo> EnumerateNetworkInterfaces(
    const bool include_virtual_interfaces) {
    std::vector<NetworkInterfaceInfo> interfaces;
    for (const auto& info : WindowsNetworkInterfaceEnumerator::Enumerate(
             include_virtual_interfaces)) {
        interfaces.push_back({
            .ipv4_interface_index = info.ipv4_interface_index,
            .ipv4_address_text = info.ipv4_address_text,
            .friendly_name = info.friendly_name,
            .description = info.description,
            .is_virtual = info.is_virtual,
        });
    }
    return interfaces;
}

}  // namespace airplaywin::app
