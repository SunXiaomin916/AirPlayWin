#include "TestFramework.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/group/GroupSyncAnalyzer.h"
#include "core/timing/ClockServo.h"
#include "core/transport/NetworkFaultInjector.h"

void TestGroupSoakRegression() {
    using airplaywin::group::GroupSyncAnalyzer;
    using airplaywin::group::GroupSyncTimestampConfig;
    using airplaywin::group::GroupSyncTimestampObservation;
    using airplaywin::timing::ClockServo;
    using airplaywin::timing::ClockServoConfig;
    using airplaywin::timing::ClockSyncSample;
    using airplaywin::transport::NetworkFaultConfig;
    using airplaywin::transport::NetworkFaultInjector;

    constexpr std::uint64_t tick_microseconds = 250'000U;
    constexpr std::uint64_t duration_microseconds = 24ULL * 60U * 60U * 1'000'000U;
    constexpr std::uint64_t tick_count = duration_microseconds / tick_microseconds;
    constexpr std::int64_t qpc_base = 10'000'000LL;
    constexpr std::int64_t qpc_frequency = 1'000'000LL;
    constexpr std::uint64_t first_master = 0x1111U;
    constexpr std::uint64_t second_master = 0x2222U;
    constexpr std::array<double, 4U> drift_ppm{-100.0, -33.0, 50.0, 100.0};
    constexpr std::array<std::int64_t, 4U> endpoint_latency_microseconds{
        -700, 0, 450, 1'000};

    const ClockServoConfig servo_config{
        .local_clock_frequency = qpc_frequency,
        .minimum_lock_samples = 8U,
        .relock_samples = 3U,
        .maximum_rtt_microseconds = 20'000U,
        .outlier_threshold_microseconds = 3'000U,
        .hard_resync_threshold_microseconds = 50'000U,
        .holdover_after_microseconds = 500'000U,
        .unlock_after_microseconds = 7'000'000U,
        .maximum_absolute_drift_ppm = 500.0,
        .drift_filter_alpha = 0.25,
        .offset_filter_alpha = 0.125,
        .maximum_rate_slew_ppm_per_second = 50.0,
    };
    std::array<std::unique_ptr<ClockServo>, 4U> servos;
    std::array<std::unique_ptr<NetworkFaultInjector>, 4U> faults;
    for (std::size_t member = 0U; member < servos.size(); ++member) {
        servos[member] = std::make_unique<ClockServo>(servo_config);
        faults[member] = std::make_unique<NetworkFaultInjector>(NetworkFaultConfig{
            .base_latency_microseconds = 1'000U,
            .jitter_microseconds = 150U,
            .random_loss_basis_points = 200U,
            .seed = 0xCAFEU + member,
        });
    }

    const auto qpc_at = [&](const std::size_t member,
                            const long double true_microseconds) {
        const auto rate = 1.0L + static_cast<long double>(drift_ppm[member]) /
                                     1'000'000.0L;
        return qpc_base + static_cast<std::int64_t>(
                              std::llround(true_microseconds * rate));
    };
    const auto in_outage = [](const std::uint64_t time_microseconds,
                              const std::uint64_t start_hour,
                              const std::uint64_t duration) {
        const auto start = start_hour * 60U * 60U * 1'000'000U;
        return time_microseconds >= start && time_microseconds < start + duration;
    };

    std::vector<GroupSyncTimestampObservation> observations;
    observations.reserve(9'000U * servos.size());
    std::uint64_t pulse_index = 0U;
    for (std::uint64_t tick = 0U; tick <= tick_count; ++tick) {
        const auto remote_microseconds = tick * tick_microseconds;
        const auto remote_nanoseconds = remote_microseconds * 1'000U;
        const auto master = remote_microseconds < duration_microseconds / 2U
                                ? first_master
                                : second_master;
        for (std::size_t member = 0U; member < servos.size(); ++member) {
            const bool forced_outage =
                (member == 1U && in_outage(remote_microseconds, 2U, 500'000U)) ||
                (member == 2U && in_outage(remote_microseconds, 4U, 2'000'000U)) ||
                (member == 3U && in_outage(remote_microseconds, 6U, 5'000'000U));
            const auto fault = faults[member]->Next(tick, remote_microseconds);
            if (!forced_outage && !fault.drop) {
                APW_EXPECT(servos[member]->AddSample(ClockSyncSample{
                    .remote_time_nanoseconds = remote_nanoseconds,
                    .local_receive_qpc = qpc_at(
                        member,
                        static_cast<long double>(fault.delivery_time_microseconds)),
                    .source_clock_identity = master,
                }));
            } else {
                servos[member]->Update(
                    qpc_at(member, static_cast<long double>(remote_microseconds)));
            }
        }

        if (remote_microseconds >= 30'000'000U && tick % 40U == 0U) {
            bool complete = true;
            std::array<std::int64_t, 4U> presentation_nanoseconds{};
            for (std::size_t member = 0U; member < servos.size(); ++member) {
                const auto mapped = servos[member]->RemoteToLocalQpc(remote_nanoseconds);
                if (!mapped.has_value()) {
                    complete = false;
                    break;
                }
                const auto corrected_qpc =
                    *mapped - endpoint_latency_microseconds[member];
                const auto rate = 1.0L + static_cast<long double>(drift_ppm[member]) /
                                             1'000'000.0L;
                const auto command_time_microseconds =
                    (static_cast<long double>(corrected_qpc) - qpc_base) / rate;
                const auto acoustic_time_microseconds =
                    command_time_microseconds + endpoint_latency_microseconds[member];
                presentation_nanoseconds[member] = static_cast<std::int64_t>(
                    std::llround(acoustic_time_microseconds * 1'000.0L));
            }
            if (complete) {
                for (std::size_t member = 0U; member < servos.size(); ++member) {
                    observations.push_back({
                        .pulse_index = pulse_index,
                        .member_id = member + 1U,
                        .presentation_time_nanoseconds =
                            presentation_nanoseconds[member],
                    });
                }
                ++pulse_index;
            }
        }
    }

    const auto result = GroupSyncAnalyzer::AnalyzeTimestamps(
        observations,
        GroupSyncTimestampConfig{.members = {1U, 2U, 3U, 4U},
                                 .minimum_complete_pulses = 8'000U});
    APW_EXPECT(result.Succeeded());
    APW_EXPECT(result.complete_pulses >= 8'000U);
    APW_EXPECT(result.group_skew.p95_microseconds <= 1'000U);
    APW_EXPECT(result.group_skew.p99_microseconds <= 2'000U);
    APW_EXPECT(result.meets_targets);
    for (const auto& servo : servos) {
        const auto diagnostics = servo->Diagnostics();
        APW_EXPECT(diagnostics.master_change_events == 1U);
        APW_EXPECT(diagnostics.relock_events >= 1U);
        APW_EXPECT(diagnostics.locked);
        APW_EXPECT(!servo->ConsumeHardResyncRequest());
    }
    APW_EXPECT(servos[1U]->Diagnostics().holdover_events >= 2U);
    APW_EXPECT(servos[2U]->Diagnostics().holdover_events >= 2U);
    APW_EXPECT(servos[3U]->Diagnostics().holdover_events >= 2U);
}
