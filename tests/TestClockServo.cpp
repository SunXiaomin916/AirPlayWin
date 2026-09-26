#include "TestFramework.h"

#include <cmath>
#include <cstdint>
#include <stdexcept>

#include "core/timing/ClockServo.h"

void TestClockServo() {
    using airplaywin::timing::ClockServo;
    using airplaywin::timing::ClockServoConfig;
    using airplaywin::timing::ClockServoState;
    using airplaywin::timing::ClockSyncSample;

    ClockServo servo{ClockServoConfig{
        .local_clock_frequency = 1'000'000LL,
        .minimum_lock_samples = 8U,
        .relock_samples = 3U,
        .maximum_rtt_microseconds = 20'000U,
        .outlier_threshold_microseconds = 3'000U,
        .hard_resync_threshold_microseconds = 50'000U,
        .holdover_after_microseconds = 1'500'000U,
        .unlock_after_microseconds = 5'000'000U,
        .maximum_absolute_drift_ppm = 500.0,
        .drift_filter_alpha = 0.25,
        .offset_filter_alpha = 0.125,
        .maximum_rate_slew_ppm_per_second = 25.0,
    }};

    constexpr std::int64_t base_qpc = 10'000'000LL;
    constexpr std::uint64_t identity = 0x0102'0304'0506'0708ULL;
    for (std::uint64_t second = 0U; second < 40U; ++second) {
        APW_EXPECT(servo.AddSample(ClockSyncSample{
            .remote_time_nanoseconds = second * 1'000'000'000ULL,
            .local_receive_qpc = base_qpc + static_cast<std::int64_t>(second * 1'000'100ULL),
            .source_clock_identity = identity,
        }));
    }
    auto diagnostics = servo.Diagnostics();
    APW_EXPECT(diagnostics.state == ClockServoState::Locked);
    APW_EXPECT(diagnostics.locked);
    APW_EXPECT(diagnostics.accepted_samples == 40U);
    APW_EXPECT(std::abs(diagnostics.drift_ppm - 100.0) < 1.0);
    APW_EXPECT(std::abs(diagnostics.rate_correction - (1.0 / 1.0001)) < 0.00001);
    const auto mapped = servo.RemoteToLocalQpc(45'000'000'000ULL);
    APW_EXPECT(mapped.has_value());
    APW_EXPECT(std::abs(*mapped - (base_qpc + 45'004'500LL)) < 100LL);

    servo.Update(base_qpc + 40'600'000LL);
    diagnostics = servo.Diagnostics();
    APW_EXPECT(diagnostics.state == ClockServoState::Holdover);
    APW_EXPECT(diagnostics.locked);
    APW_EXPECT(diagnostics.holdover_events == 1U);
    APW_EXPECT(servo.RemoteToLocalQpc(41'000'000'000ULL).has_value());

    for (std::uint64_t second = 41U; second <= 43U; ++second) {
        APW_EXPECT(servo.AddSample(ClockSyncSample{
            .remote_time_nanoseconds = second * 1'000'000'000ULL,
            .local_receive_qpc = base_qpc + static_cast<std::int64_t>(second * 1'000'100ULL),
            .source_clock_identity = identity,
        }));
    }
    diagnostics = servo.Diagnostics();
    APW_EXPECT(diagnostics.state == ClockServoState::Locked);
    APW_EXPECT(diagnostics.relock_events == 1U);

    APW_EXPECT(!servo.AddSample(ClockSyncSample{
        .remote_time_nanoseconds = 44'000'000'000ULL,
        .local_receive_qpc = base_qpc + 44'004'400LL + 10'000LL,
        .source_clock_identity = identity,
    }));
    APW_EXPECT(servo.Diagnostics().outlier_samples == 1U);

    APW_EXPECT(servo.AddSample(ClockSyncSample{
        .remote_time_nanoseconds = 45'000'000'000ULL,
        .local_receive_qpc = base_qpc + 45'004'500LL + 100'000LL,
        .source_clock_identity = identity,
    }));
    diagnostics = servo.Diagnostics();
    APW_EXPECT(diagnostics.hard_resync_events == 1U);
    APW_EXPECT(diagnostics.state == ClockServoState::Acquiring);
    APW_EXPECT(servo.ConsumeHardResyncRequest());
    APW_EXPECT(!servo.ConsumeHardResyncRequest());

    servo.Update(base_qpc + 51'000'000LL);
    APW_EXPECT(servo.Diagnostics().state == ClockServoState::Unlocked);
    APW_EXPECT(!servo.RemoteToLocalQpc(51'000'000'000ULL).has_value());
    APW_EXPECT(servo.AddSample(ClockSyncSample{
        .remote_time_nanoseconds = 51'000'000'000ULL,
        .local_receive_qpc = base_qpc + 51'005'100LL,
        .source_clock_identity = identity,
    }));
    APW_EXPECT(servo.Diagnostics().state == ClockServoState::Acquiring);
    APW_EXPECT(servo.Diagnostics().accepted_samples == 45U);

    ClockServo jittered{ClockServoConfig{
        .local_clock_frequency = 1'000'000LL,
        .minimum_lock_samples = 8U,
        .relock_samples = 3U,
        .maximum_rtt_microseconds = 20'000U,
        .outlier_threshold_microseconds = 3'000U,
        .hard_resync_threshold_microseconds = 50'000U,
        .holdover_after_microseconds = 1'500'000U,
        .unlock_after_microseconds = 5'000'000U,
        .maximum_absolute_drift_ppm = 500.0,
        .drift_filter_alpha = 0.25,
        .offset_filter_alpha = 0.125,
        .maximum_rate_slew_ppm_per_second = 25.0,
    }};
    for (std::uint64_t sample = 0U; sample < 40U; ++sample) {
        const auto jitter = (sample & 1U) == 0U ? 500LL : -500LL;
        APW_EXPECT(jittered.AddSample(ClockSyncSample{
            .remote_time_nanoseconds = sample * 125'000'000ULL,
            .local_receive_qpc = base_qpc + static_cast<std::int64_t>(sample * 125'012ULL) +
                                 jitter,
            .source_clock_identity = identity,
        }));
    }
    const auto jittered_diagnostics = jittered.Diagnostics();
    APW_EXPECT(jittered_diagnostics.state == ClockServoState::Locked);
    APW_EXPECT(jittered_diagnostics.outlier_samples == 0U);
    APW_EXPECT(std::abs(jittered_diagnostics.drift_ppm - 100.0) < 150.0);
    APW_EXPECT(jittered.AddSample(ClockSyncSample{
        .remote_time_nanoseconds = 5'000'000'000ULL,
        .local_receive_qpc = base_qpc + 5'000'500LL,
        .source_clock_identity = identity + 1U,
    }));
    const auto changed_master = jittered.Diagnostics();
    APW_EXPECT(changed_master.master_change_events == 1U);
    APW_EXPECT(changed_master.source_clock_identity == identity);
    APW_EXPECT(changed_master.state == ClockServoState::Holdover);
    APW_EXPECT(changed_master.locked);
    const auto holdover_target = jittered.RemoteToLocalQpc(5'125'000'000ULL);
    APW_EXPECT(holdover_target.has_value());
    for (std::uint64_t sample = 1U; sample <= 3U; ++sample) {
        APW_EXPECT(jittered.AddSample(ClockSyncSample{
            .remote_time_nanoseconds = 5'000'000'000ULL + sample * 125'000'000ULL,
            .local_receive_qpc = base_qpc + 5'000'500LL +
                                 static_cast<std::int64_t>(sample * 125'012ULL),
            .source_clock_identity = identity + 1U,
        }));
    }
    const auto relocked_master = jittered.Diagnostics();
    APW_EXPECT(relocked_master.source_clock_identity == identity + 1U);
    APW_EXPECT(relocked_master.state == ClockServoState::Locked);
    APW_EXPECT(relocked_master.locked);
    APW_EXPECT(relocked_master.relock_events == 1U);
    const auto relocked_target = jittered.RemoteToLocalQpc(5'500'000'000ULL);
    APW_EXPECT(relocked_target.has_value());
    APW_EXPECT(!jittered.ConsumeHardResyncRequest());

    constexpr std::uint64_t third_identity = identity + 2U;
    for (std::uint64_t sample = 0U; sample <= 3U; ++sample) {
        APW_EXPECT(jittered.AddSample(ClockSyncSample{
            .remote_time_nanoseconds = 6'000'000'000ULL + sample * 125'000'000ULL,
            .local_receive_qpc = base_qpc + 6'000'600LL + 100'000LL +
                                 static_cast<std::int64_t>(sample * 125'012ULL),
            .source_clock_identity = third_identity,
        }));
    }
    APW_EXPECT(jittered.Diagnostics().source_clock_identity == third_identity);
    APW_EXPECT(jittered.Diagnostics().hard_resync_events == 1U);
    APW_EXPECT(jittered.ConsumeHardResyncRequest());

    bool rejected = false;
    try {
        static_cast<void>(ClockServo{ClockServoConfig{}});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    APW_EXPECT(rejected);
}
