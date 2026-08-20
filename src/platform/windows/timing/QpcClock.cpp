#include "platform/windows/timing/QpcClock.h"

#include <Windows.h>

namespace airplaywin::windows::timing {

std::int64_t QpcClock::Now() noexcept {
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value) != FALSE ? value.QuadPart : 0;
}

std::int64_t QpcClock::Frequency() noexcept {
    static const std::int64_t frequency = []() noexcept {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) != FALSE ? value.QuadPart : 0LL;
    }();
    return frequency;
}

double QpcClock::ToSeconds(const std::int64_t ticks) noexcept {
    const auto frequency = Frequency();
    return frequency > 0 ? static_cast<double>(ticks) / static_cast<double>(frequency) : 0.0;
}

}  // namespace airplaywin::windows::timing
