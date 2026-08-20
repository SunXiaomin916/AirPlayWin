#pragma once

#include <cstdint>

#include "core/timing/IMonotonicClock.h"

namespace airplaywin::windows::timing {

class QpcClock final {
public:
    [[nodiscard]] static std::int64_t Now() noexcept;
    [[nodiscard]] static std::int64_t Frequency() noexcept;
    [[nodiscard]] static double ToSeconds(std::int64_t ticks) noexcept;
};

class QpcClockSource final : public airplaywin::timing::IMonotonicClock {
public:
    [[nodiscard]] std::int64_t Now() const noexcept override;
    [[nodiscard]] std::int64_t Frequency() const noexcept override;
};

}  // namespace airplaywin::windows::timing
