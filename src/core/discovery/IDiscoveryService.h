#pragma once

#include "core/discovery/DiscoveryTypes.h"

namespace airplaywin::discovery {

class IDiscoveryService {
public:
    virtual ~IDiscoveryService() = default;

    [[nodiscard]] virtual bool Start(const DiscoveryConfig& config) = 0;
    virtual void Stop() noexcept = 0;
    [[nodiscard]] virtual DiscoveryDiagnostics Diagnostics() const = 0;
};

}  // namespace airplaywin::discovery
