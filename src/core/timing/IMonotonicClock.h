#pragma once

#include <cstdint>

namespace airplaywin::timing {

class IMonotonicClock {
public:
    virtual ~IMonotonicClock() = default;

    [[nodiscard]] virtual std::int64_t Now() const noexcept = 0;
    [[nodiscard]] virtual std::int64_t Frequency() const noexcept = 0;
};

}  // namespace airplaywin::timing
