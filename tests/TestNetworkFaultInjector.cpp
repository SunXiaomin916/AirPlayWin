#include "TestFramework.h"

#include <cstdint>
#include <stdexcept>

#include "core/transport/NetworkFaultInjector.h"

void TestNetworkFaultInjector() {
    using airplaywin::transport::NetworkFaultConfig;
    using airplaywin::transport::NetworkFaultInjector;

    const NetworkFaultConfig config{
        .base_latency_microseconds = 50'000U,
        .jitter_microseconds = 100'000U,
        .random_loss_basis_points = 500U,
        .duplicate_basis_points = 200U,
        .reorder_basis_points = 300U,
        .burst_period_packets = 100U,
        .burst_length_packets = 5U,
        .reorder_delay_microseconds = 25'000U,
        .seed = 0x1234'5678U,
    };
    NetworkFaultInjector first{config};
    NetworkFaultInjector second{config};
    for (std::uint64_t packet = 0U; packet < 10'000U; ++packet) {
        const auto source_time = packet * 8'000U;
        const auto a = first.Next(packet, source_time);
        const auto b = second.Next(packet, source_time);
        APW_EXPECT(a.drop == b.drop);
        APW_EXPECT(a.duplicate == b.duplicate);
        APW_EXPECT(a.reorder == b.reorder);
        APW_EXPECT(a.burst_drop == b.burst_drop);
        APW_EXPECT(a.delivery_time_microseconds == b.delivery_time_microseconds);
        APW_EXPECT(a.delivery_time_microseconds >= source_time);
        APW_EXPECT(a.delivery_time_microseconds - source_time <= 175'000U);
    }
    const auto diagnostics = first.Diagnostics();
    APW_EXPECT(diagnostics.packets_evaluated == 10'000U);
    APW_EXPECT(diagnostics.burst_drops == 496U);
    APW_EXPECT(diagnostics.random_drops > 300U);
    APW_EXPECT(diagnostics.duplicated_packets > 100U);
    APW_EXPECT(diagnostics.reordered_packets > 150U);
    APW_EXPECT(diagnostics.minimum_delay_microseconds == 0U);
    APW_EXPECT(diagnostics.maximum_delay_microseconds <= 175'000U);

    first.Reset();
    second.Reset();
    const auto reset_a = first.Next(0U, 0U);
    const auto reset_b = second.Next(0U, 0U);
    APW_EXPECT(reset_a.delivery_time_microseconds == reset_b.delivery_time_microseconds);
    APW_EXPECT(reset_a.drop == reset_b.drop);

    bool invalid_rejected = false;
    try {
        static_cast<void>(NetworkFaultInjector{
            NetworkFaultConfig{.jitter_microseconds = 100'001U}});
    } catch (const std::invalid_argument&) {
        invalid_rejected = true;
    }
    APW_EXPECT(invalid_rejected);
}
