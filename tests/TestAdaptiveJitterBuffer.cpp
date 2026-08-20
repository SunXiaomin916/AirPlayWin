#include "TestFramework.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "RtpTestUtils.h"
#include "core/transport/RtpJitterBuffer.h"

namespace {

void InsertPacket(airplaywin::transport::RtpJitterBuffer& buffer,
                  const std::uint16_t sequence,
                  const std::uint32_t timestamp,
                  const std::int64_t arrival_ns) {
    const std::array payload{std::byte{0}, std::byte{1}};
    const auto bytes = airplaywin::tests::BuildRtpPacket(sequence, timestamp, payload);
    const auto packet = airplaywin::transport::ParseRtpPacket(bytes);
    APW_EXPECT(packet.has_value());
    APW_EXPECT(buffer.Insert(*packet, arrival_ns));
}

void PopPacket(airplaywin::transport::RtpJitterBuffer& buffer,
               const std::uint16_t expected_sequence) {
    airplaywin::transport::BufferedRtpPacket output;
    APW_EXPECT(buffer.Pop(output) == airplaywin::transport::JitterPopKind::Packet);
    APW_EXPECT(output.sequence_number == expected_sequence);
}

}  // namespace

void TestAdaptiveJitterBuffer() {
    using airplaywin::transport::AdaptiveJitterState;
    using airplaywin::transport::RtpJitterBuffer;
    using airplaywin::transport::RtpJitterBufferConfig;

    const RtpJitterBufferConfig config{
        .capacity_packets = 32U,
        .target_packets = 4U,
        .clock_rate = 48'000U,
        .adaptive_enabled = true,
        .minimum_target_packets = 2U,
        .maximum_target_packets = 8U,
        .stable_window_packets = 8U,
        .recovery_window_packets = 4U,
    };
    RtpJitterBuffer stable{config};
    std::uint16_t sequence = 1U;
    std::uint32_t timestamp = 1'000U;
    std::int64_t arrival = 1'000'000'000LL;
    for (std::uint16_t index = 0U; index < 4U; ++index) {
        InsertPacket(stable, sequence++, timestamp, arrival);
        timestamp += 480U;
        arrival += 10'000'000LL;
    }
    for (std::uint16_t expected = 1U; expected <= 4U; ++expected) {
        PopPacket(stable, expected);
    }
    for (std::uint16_t index = 0U; index < 20U; ++index) {
        InsertPacket(stable, sequence, timestamp, arrival);
        PopPacket(stable, sequence);
        ++sequence;
        timestamp += 480U;
        arrival += 10'000'000LL;
    }
    auto diagnostics = stable.Diagnostics();
    APW_EXPECT(diagnostics.adaptive_enabled);
    APW_EXPECT(diagnostics.target_packets == 2U);
    APW_EXPECT(diagnostics.target_decrease_events == 2U);
    APW_EXPECT(diagnostics.adaptive_state == AdaptiveJitterState::LowLatency);

    stable.ReportDownstreamUnderrun(2U);
    diagnostics = stable.Diagnostics();
    APW_EXPECT(diagnostics.target_packets >= 4U);
    APW_EXPECT(diagnostics.adaptive_state == AdaptiveJitterState::Degraded);
    APW_EXPECT(diagnostics.downstream_underruns == 2U);
    APW_EXPECT(diagnostics.degradation_events >= 1U);

    RtpJitterBuffer jitter_burst{config};
    sequence = 100U;
    timestamp = 10'000U;
    arrival = 2'000'000'000LL;
    for (std::uint16_t index = 0U; index < 4U; ++index) {
        InsertPacket(jitter_burst, sequence++, timestamp, arrival);
        timestamp += 480U;
        arrival += 10'000'000LL;
    }
    for (std::uint16_t expected = 100U; expected < 104U; ++expected) {
        PopPacket(jitter_burst, expected);
    }
    for (std::uint16_t index = 0U; index < 32U; ++index) {
        arrival += index == 8U ? 30'000'000LL : 10'000'000LL;
        InsertPacket(jitter_burst, sequence, timestamp, arrival);
        PopPacket(jitter_burst, sequence);
        ++sequence;
        timestamp += 480U;
    }
    diagnostics = jitter_burst.Diagnostics();
    APW_EXPECT(diagnostics.jitter_p99_microseconds >= 19'000U);
    APW_EXPECT(diagnostics.target_packets >= 4U);
    APW_EXPECT(diagnostics.target_increase_events >= 1U);
}
