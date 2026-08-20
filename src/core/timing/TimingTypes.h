#pragma once

#include <cstdint>

namespace airplaywin::timing {

struct TimingDiagnostics final {
    bool enabled{false};
    bool locked{false};
    std::uint64_t generation{0U};
    std::uint32_t remote_clock_rate{0U};
    std::uint64_t target_buffer_microseconds{0U};
    std::uint64_t mapped_packets{0U};
    std::uint64_t late_mappings{0U};
    std::uint64_t maximum_lateness_microseconds{0U};
    std::uint64_t anchor_remote_time{0U};
    std::int64_t anchor_target_qpc{0};
    std::uint64_t last_remote_time{0U};
    std::int64_t last_target_qpc{0};
    std::uint32_t last_error{0U};
};

}  // namespace airplaywin::timing
