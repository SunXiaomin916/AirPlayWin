#pragma once

#include <cstdint>

namespace airplaywin::windows::timing {

class QpcClock final {
public:
    [[nodiscard]] static std::int64_t Now() noexcept;
    [[nodiscard]] static std::int64_t Frequency() noexcept;
    [[nodiscard]] static double ToSeconds(std::int64_t ticks) noexcept;
};

}  // namespace airplaywin::windows::timing
