#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace airplaywin::windows::system {

enum class FirewallProtocol : std::uint8_t {
    Tcp = 6U,
    Udp = 17U,
};

enum class FirewallRuleHealth : std::uint8_t {
    Missing,
    Healthy,
    Misconfigured,
    Error,
};

struct FirewallRuleSpec final {
    std::wstring name{};
    std::wstring description{};
    std::wstring executable_path{};
    FirewallProtocol protocol{FirewallProtocol::Tcp};
    std::wstring local_ports{};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct FirewallRuleStatus final {
    FirewallRuleHealth health{FirewallRuleHealth::Error};
    bool enabled{false};
    bool private_profile_only{false};
    bool inbound_allow{false};
    bool executable_matches{false};
    bool protocol_matches{false};
    bool ports_match{false};
    std::uint32_t last_error{0U};
};

class WindowsFirewallManager final {
public:
    [[nodiscard]] static std::wstring CurrentExecutablePath();
    [[nodiscard]] static std::vector<FirewallRuleSpec> CoreRuleSpecs(
        const std::wstring& executable_path,
        std::uint16_t raop_port,
        std::uint16_t airplay_port);
    [[nodiscard]] static FirewallRuleStatus Query(const FirewallRuleSpec& spec) noexcept;
    [[nodiscard]] static bool Install(const FirewallRuleSpec& spec,
                                      std::uint32_t& error) noexcept;
    [[nodiscard]] static bool Remove(const std::wstring& rule_name,
                                     std::uint32_t& error) noexcept;
};

}  // namespace airplaywin::windows::system
