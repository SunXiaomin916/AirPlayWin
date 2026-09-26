#include "platform/windows/system/WindowsFirewallManager.h"

#include <Windows.h>
#include <netfw.h>
#include <wrl/client.h>

#include <filesystem>
#include <memory>
#include <string>

namespace airplaywin::windows::system {

namespace {

using Microsoft::WRL::ComPtr;

struct BstrDeleter final {
    void operator()(wchar_t* const value) const noexcept {
        if (value != nullptr) {
            SysFreeString(value);
        }
    }
};

using UniqueBstr = std::unique_ptr<wchar_t, BstrDeleter>;

[[nodiscard]] UniqueBstr MakeBstr(const std::wstring& value) noexcept {
    return UniqueBstr{SysAllocStringLen(value.data(), static_cast<UINT>(value.size()))};
}

class ComApartment final {
public:
    ComApartment() noexcept {
        result_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        uninitialize_ = SUCCEEDED(result_);
    }

    ~ComApartment() {
        if (uninitialize_) {
            CoUninitialize();
        }
    }

    [[nodiscard]] bool Available() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

    [[nodiscard]] HRESULT Result() const noexcept { return result_; }

private:
    HRESULT result_{E_FAIL};
    bool uninitialize_{false};
};

[[nodiscard]] std::uint32_t ErrorFromHresult(const HRESULT result) noexcept {
    return static_cast<std::uint32_t>(result);
}

[[nodiscard]] bool IsMissingRule(const HRESULT result) noexcept {
    return result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

[[nodiscard]] bool EqualOrdinalInsensitive(const std::wstring& left,
                                           const std::wstring& right) noexcept {
    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(),
                                static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] HRESULT GetRules(ComPtr<INetFwRules>& rules) noexcept {
    ComPtr<INetFwPolicy2> policy;
    auto result = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&policy));
    if (FAILED(result)) {
        return result;
    }
    return policy->get_Rules(&rules);
}

}  // namespace

bool FirewallRuleSpec::IsValid() const noexcept {
    if (name.empty() || name == L"all" || name.find(L'|') != std::wstring::npos ||
        executable_path.empty()) {
        return false;
    }
    try {
        return std::filesystem::path{executable_path}.is_absolute();
    } catch (...) {
        return false;
    }
}

std::wstring WindowsFirewallManager::CurrentExecutablePath() {
    std::wstring path(32'768U, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0U || length >= path.size()) {
        return {};
    }
    path.resize(length);
    return path;
}

std::vector<FirewallRuleSpec> WindowsFirewallManager::CoreRuleSpecs(
    const std::wstring& executable_path,
    const std::uint16_t raop_port,
    const std::uint16_t airplay_port,
    const bool include_experimental_airplay) {
    const auto tcp_ports = include_experimental_airplay
                               ? std::to_wstring(raop_port) + L"," +
                                     std::to_wstring(airplay_port)
                               : std::to_wstring(raop_port);
    return {
        FirewallRuleSpec{.name = L"AirPlayWin Core TCP",
                         .description =
                             L"AirPlayWin RAOP control listener (Private only)",
                         .executable_path = executable_path,
                         .protocol = FirewallProtocol::Tcp,
                         .local_ports = tcp_ports},
        FirewallRuleSpec{.name = L"AirPlayWin Core UDP",
                         .description =
                             L"AirPlayWin negotiated RTP/control/timing UDP (Private only)",
                         .executable_path = executable_path,
                         .protocol = FirewallProtocol::Udp,
                         .local_ports = {}},
    };
}

FirewallRuleStatus WindowsFirewallManager::Query(const FirewallRuleSpec& spec) noexcept {
    if (!spec.IsValid()) {
        return {.health = FirewallRuleHealth::Error, .last_error = ERROR_INVALID_PARAMETER};
    }
    ComApartment apartment;
    if (!apartment.Available()) {
        return {.health = FirewallRuleHealth::Error,
                .last_error = ErrorFromHresult(apartment.Result())};
    }
    ComPtr<INetFwRules> rules;
    auto result = GetRules(rules);
    if (FAILED(result)) {
        return {.health = FirewallRuleHealth::Error,
                .last_error = ErrorFromHresult(result)};
    }
    const auto name = MakeBstr(spec.name);
    if (!name) {
        return {.health = FirewallRuleHealth::Error, .last_error = ERROR_NOT_ENOUGH_MEMORY};
    }
    ComPtr<INetFwRule> rule;
    result = rules->Item(name.get(), &rule);
    if (IsMissingRule(result)) {
        return {.health = FirewallRuleHealth::Missing};
    }
    if (FAILED(result)) {
        return {.health = FirewallRuleHealth::Error,
                .last_error = ErrorFromHresult(result)};
    }

    VARIANT_BOOL enabled = VARIANT_FALSE;
    long profiles = 0;
    NET_FW_ACTION action = NET_FW_ACTION_BLOCK;
    NET_FW_RULE_DIRECTION direction = NET_FW_RULE_DIR_OUT;
    long protocol = 0;
    BSTR raw_application = nullptr;
    BSTR raw_ports = nullptr;
    result = rule->get_Enabled(&enabled);
    result = SUCCEEDED(result) ? rule->get_Profiles(&profiles) : result;
    result = SUCCEEDED(result) ? rule->get_Action(&action) : result;
    result = SUCCEEDED(result) ? rule->get_Direction(&direction) : result;
    result = SUCCEEDED(result) ? rule->get_Protocol(&protocol) : result;
    result = SUCCEEDED(result) ? rule->get_ApplicationName(&raw_application) : result;
    if (SUCCEEDED(result)) {
        const auto ports_result = rule->get_LocalPorts(&raw_ports);
        if (FAILED(ports_result) && !spec.local_ports.empty()) {
            result = ports_result;
        }
    }
    const UniqueBstr application{raw_application};
    const UniqueBstr ports{raw_ports};
    if (FAILED(result)) {
        return {.health = FirewallRuleHealth::Error,
                .last_error = ErrorFromHresult(result)};
    }

    const std::wstring application_value = application ? application.get() : L"";
    const std::wstring ports_value = ports ? ports.get() : L"";
    FirewallRuleStatus status{
        .health = FirewallRuleHealth::Misconfigured,
        .enabled = enabled == VARIANT_TRUE,
        .private_profile_only = profiles == NET_FW_PROFILE2_PRIVATE,
        .inbound_allow = direction == NET_FW_RULE_DIR_IN && action == NET_FW_ACTION_ALLOW,
        .executable_matches = EqualOrdinalInsensitive(application_value, spec.executable_path),
        .protocol_matches = protocol == static_cast<long>(spec.protocol),
        .ports_match = spec.local_ports.empty()
                           ? ports_value.empty() || ports_value == L"*"
                           : ports_value == spec.local_ports,
    };
    if (status.enabled && status.private_profile_only && status.inbound_allow &&
        status.executable_matches && status.protocol_matches && status.ports_match) {
        status.health = FirewallRuleHealth::Healthy;
    }
    return status;
}

bool WindowsFirewallManager::Install(const FirewallRuleSpec& spec,
                                     std::uint32_t& error) noexcept {
    error = ERROR_SUCCESS;
    if (!spec.IsValid()) {
        error = ERROR_INVALID_PARAMETER;
        return false;
    }
    if (Query(spec).health == FirewallRuleHealth::Healthy) {
        return true;
    }
    ComApartment apartment;
    if (!apartment.Available()) {
        error = ErrorFromHresult(apartment.Result());
        return false;
    }
    ComPtr<INetFwRules> rules;
    auto result = GetRules(rules);
    if (FAILED(result)) {
        error = ErrorFromHresult(result);
        return false;
    }
    const auto name = MakeBstr(spec.name);
    const auto description = MakeBstr(spec.description);
    const auto application = MakeBstr(spec.executable_path);
    const auto grouping = MakeBstr(std::wstring{L"AirPlayWin"});
    if (!name || !description || !application || !grouping) {
        error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    result = rules->Remove(name.get());
    if (FAILED(result) && !IsMissingRule(result)) {
        error = ErrorFromHresult(result);
        return false;
    }

    ComPtr<INetFwRule> rule;
    result = CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&rule));
    result = SUCCEEDED(result) ? rule->put_Name(name.get()) : result;
    result = SUCCEEDED(result) ? rule->put_Description(description.get()) : result;
    result = SUCCEEDED(result) ? rule->put_ApplicationName(application.get()) : result;
    result = SUCCEEDED(result)
                 ? rule->put_Protocol(static_cast<long>(spec.protocol))
                 : result;
    UniqueBstr ports;
    if (SUCCEEDED(result) && !spec.local_ports.empty()) {
        ports = MakeBstr(spec.local_ports);
        result = ports ? rule->put_LocalPorts(ports.get()) : E_OUTOFMEMORY;
    }
    result = SUCCEEDED(result) ? rule->put_Direction(NET_FW_RULE_DIR_IN) : result;
    result = SUCCEEDED(result) ? rule->put_Action(NET_FW_ACTION_ALLOW) : result;
    result = SUCCEEDED(result) ? rule->put_Profiles(NET_FW_PROFILE2_PRIVATE) : result;
    result = SUCCEEDED(result) ? rule->put_Enabled(VARIANT_TRUE) : result;
    result = SUCCEEDED(result) ? rule->put_EdgeTraversal(VARIANT_FALSE) : result;
    result = SUCCEEDED(result) ? rule->put_Grouping(grouping.get()) : result;
    result = SUCCEEDED(result) ? rules->Add(rule.Get()) : result;
    if (FAILED(result)) {
        error = ErrorFromHresult(result);
        return false;
    }
    return true;
}

bool WindowsFirewallManager::Remove(const std::wstring& rule_name,
                                    std::uint32_t& error) noexcept {
    error = ERROR_SUCCESS;
    if (rule_name.empty()) {
        error = ERROR_INVALID_PARAMETER;
        return false;
    }
    ComApartment apartment;
    if (!apartment.Available()) {
        error = ErrorFromHresult(apartment.Result());
        return false;
    }
    ComPtr<INetFwRules> rules;
    auto result = GetRules(rules);
    if (FAILED(result)) {
        error = ErrorFromHresult(result);
        return false;
    }
    const auto name = MakeBstr(rule_name);
    if (!name) {
        error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    result = rules->Remove(name.get());
    if (SUCCEEDED(result) || IsMissingRule(result)) {
        return true;
    }
    error = ErrorFromHresult(result);
    return false;
}

}  // namespace airplaywin::windows::system
