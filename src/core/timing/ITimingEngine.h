#pragma once

#include <cstdint>
#include <optional>

#include "core/timing/TimingTypes.h"

namespace airplaywin::timing {

class ITimingEngine {
public:
    virtual ~ITimingEngine() = default;

    virtual void Reset(std::optional<std::uint64_t> remote_time) noexcept = 0;
    [[nodiscard]] virtual std::optional<std::int64_t> RemoteToLocalQpc(
        std::uint64_t remote_time) noexcept = 0;
    [[nodiscard]] virtual TimingDiagnostics Diagnostics() const noexcept = 0;
    [[nodiscard]] virtual double RateCorrection() const noexcept { return 1.0; }
    [[nodiscard]] virtual bool ConsumeHardResyncRequest() noexcept { return false; }
};

}  // namespace airplaywin::timing
