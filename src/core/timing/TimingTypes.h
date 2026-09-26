#pragma once

#include <cstdint>

namespace airplaywin::timing {

enum class ClockServoState : std::uint8_t {
    Disabled,
    Unlocked,
    Acquiring,
    Locked,
    Holdover,
    Relocking,
};

struct ClockServoDiagnostics final {
    ClockServoState state{ClockServoState::Disabled};
    bool locked{false};
    bool hard_resync_pending{false};
    std::uint64_t generation{0U};
    std::uint64_t source_clock_identity{0U};
    std::uint64_t accepted_samples{0U};
    std::uint64_t rejected_samples{0U};
    std::uint64_t outlier_samples{0U};
    std::uint64_t lock_events{0U};
    std::uint64_t holdover_events{0U};
    std::uint64_t relock_events{0U};
    std::uint64_t master_change_events{0U};
    std::uint64_t hard_resync_events{0U};
    std::int64_t offset_microseconds{0};
    double drift_ppm{0.0};
    double rate_correction{1.0};
    std::uint64_t rtt_microseconds{0U};
    std::uint64_t uncertainty_microseconds{0U};
    std::uint64_t last_sync_age_microseconds{0U};
    std::uint64_t anchor_remote_nanoseconds{0U};
    std::int64_t anchor_local_qpc{0};
    std::uint32_t last_error{0U};
};

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
    ClockServoDiagnostics servo{};
    std::uint32_t last_error{0U};
};

}  // namespace airplaywin::timing
