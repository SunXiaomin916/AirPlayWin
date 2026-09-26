#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

#include "core/timing/TimingTypes.h"

namespace airplaywin::timing {

struct ClockServoConfig final {
    std::int64_t local_clock_frequency{0};
    std::uint32_t minimum_lock_samples{8U};
    std::uint32_t relock_samples{3U};
    std::uint64_t maximum_rtt_microseconds{20'000U};
    std::uint64_t outlier_threshold_microseconds{3'000U};
    std::uint64_t hard_resync_threshold_microseconds{50'000U};
    std::uint64_t holdover_after_microseconds{1'500'000U};
    std::uint64_t unlock_after_microseconds{5'000'000U};
    double maximum_absolute_drift_ppm{500.0};
    double drift_filter_alpha{0.25};
    double offset_filter_alpha{0.125};
    double maximum_rate_slew_ppm_per_second{25.0};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct ClockSyncSample final {
    std::uint64_t remote_time_nanoseconds{0U};
    std::int64_t local_receive_qpc{0};
    std::uint64_t round_trip_nanoseconds{0U};
    std::uint64_t source_clock_identity{0U};
};

class ClockServo final {
public:
    explicit ClockServo(ClockServoConfig config);

    ClockServo(const ClockServo&) = delete;
    ClockServo& operator=(const ClockServo&) = delete;

    [[nodiscard]] bool AddSample(const ClockSyncSample& sample) noexcept;
    void Update(std::int64_t now_qpc) noexcept;
    void Reset() noexcept;
    [[nodiscard]] std::optional<std::int64_t> RemoteToLocalQpc(
        std::uint64_t remote_time_nanoseconds) const noexcept;
    [[nodiscard]] double RateCorrection() const noexcept;
    [[nodiscard]] bool ConsumeHardResyncRequest() noexcept;
    [[nodiscard]] ClockServoDiagnostics Diagnostics() const noexcept;

private:
    struct HoldoverModel final {
        bool valid{false};
        std::uint64_t source_clock_identity{0U};
        std::uint64_t anchor_remote_nanoseconds{0U};
        std::int64_t anchor_local_qpc{0};
        double drift_ppm{0.0};
        double rate_correction{1.0};
    };

    void ResetModelLocked(const ClockSyncSample& sample,
                          std::int64_t corrected_local_qpc) noexcept;
    void BeginMasterTransitionLocked(const ClockSyncSample& sample,
                                     std::int64_t corrected_local_qpc) noexcept;
    void CompleteMasterTransitionLocked(std::uint64_t remote_time_nanoseconds) noexcept;
    void UpdateStateLocked(std::int64_t now_qpc) noexcept;
    void UpdateRateCorrectionLocked(std::int64_t now_qpc) noexcept;
    [[nodiscard]] std::optional<std::int64_t> MapLocked(
        std::uint64_t remote_time_nanoseconds) const noexcept;
    [[nodiscard]] std::optional<std::int64_t> MapCurrentModelLocked(
        std::uint64_t remote_time_nanoseconds) const noexcept;
    [[nodiscard]] std::optional<std::int64_t> MapHoldoverModelLocked(
        std::uint64_t remote_time_nanoseconds) const noexcept;
    [[nodiscard]] std::uint64_t TicksToMicroseconds(std::int64_t ticks) const noexcept;

    ClockServoConfig config_{};
    mutable std::mutex mutex_{};
    ClockServoDiagnostics diagnostics_{};
    bool have_model_{false};
    std::uint64_t last_remote_nanoseconds_{0U};
    std::int64_t last_local_qpc_{0};
    std::uint64_t rate_anchor_remote_nanoseconds_{0U};
    std::int64_t rate_anchor_local_qpc_{0};
    std::int64_t last_rate_update_qpc_{0};
    std::uint32_t acquisition_samples_{0U};
    std::uint32_t relock_good_samples_{0U};
    bool master_transition_active_{false};
    std::uint64_t pending_master_clock_identity_{0U};
    HoldoverModel holdover_model_{};
};

}  // namespace airplaywin::timing
