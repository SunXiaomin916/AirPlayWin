#pragma once

#include <memory>

#include "core/discovery/IDiscoveryService.h"

namespace airplaywin::windows::network {

class WindowsDiscoveryService final : public discovery::IDiscoveryService {
public:
    WindowsDiscoveryService();
    ~WindowsDiscoveryService() override;

    WindowsDiscoveryService(const WindowsDiscoveryService&) = delete;
    WindowsDiscoveryService& operator=(const WindowsDiscoveryService&) = delete;
    WindowsDiscoveryService(WindowsDiscoveryService&&) = delete;
    WindowsDiscoveryService& operator=(WindowsDiscoveryService&&) = delete;

    [[nodiscard]] bool Start(const discovery::DiscoveryConfig& config) override;
    void Stop() noexcept override;
    [[nodiscard]] discovery::DiscoveryDiagnostics Diagnostics() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace airplaywin::windows::network
