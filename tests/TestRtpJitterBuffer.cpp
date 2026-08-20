#include "TestFramework.h"

#include <array>
#include <cstddef>

#include "RtpTestUtils.h"
#include "core/transport/RtpJitterBuffer.h"

namespace {

[[nodiscard]] bool Insert(airplaywin::transport::RtpJitterBuffer& buffer,
                          const std::uint16_t sequence,
                          const std::uint32_t timestamp,
                          const std::int64_t arrival_ns) {
    const std::array payload{std::byte{0}, std::byte{1}};
    const auto bytes = airplaywin::tests::BuildRtpPacket(sequence, timestamp, payload);
    const auto packet = airplaywin::transport::ParseRtpPacket(bytes);
    APW_EXPECT(packet.has_value());
    return buffer.Insert(*packet, arrival_ns);
}

}  // namespace

void TestRtpJitterBuffer() {
    using airplaywin::transport::BufferedRtpPacket;
    using airplaywin::transport::JitterPopKind;
    using airplaywin::transport::RtpJitterBuffer;
    using airplaywin::transport::RtpJitterBufferConfig;

    RtpJitterBuffer reordered{RtpJitterBufferConfig{
        .capacity_packets = 8U, .target_packets = 3U, .clock_rate = 44'100U}};
    APW_EXPECT(Insert(reordered, 10U, 1'000U, 1'000'000'000LL));
    APW_EXPECT(Insert(reordered, 12U, 1'704U, 1'016'000'000LL));
    APW_EXPECT(Insert(reordered, 11U, 1'352U, 1'008'000'000LL));
    APW_EXPECT(!Insert(reordered, 12U, 1'704U, 1'016'100'000LL));

    BufferedRtpPacket output;
    APW_EXPECT(reordered.Pop(output) == JitterPopKind::Packet);
    APW_EXPECT(output.sequence_number == 10U);
    APW_EXPECT(reordered.Pop(output) == JitterPopKind::Packet);
    APW_EXPECT(output.sequence_number == 11U);
    APW_EXPECT(reordered.Pop(output) == JitterPopKind::Packet);
    APW_EXPECT(output.sequence_number == 12U);
    APW_EXPECT(reordered.Pop(output) == JitterPopKind::Waiting);
    const auto reorder_diagnostics = reordered.Diagnostics();
    APW_EXPECT(reorder_diagnostics.reordered_packets == 1U);
    APW_EXPECT(reorder_diagnostics.duplicate_packets == 1U);
    APW_EXPECT(reorder_diagnostics.emitted_packets == 3U);

    RtpJitterBuffer missing{RtpJitterBufferConfig{
        .capacity_packets = 8U, .target_packets = 2U, .clock_rate = 44'100U}};
    APW_EXPECT(Insert(missing, 20U, 2'000U, 2'000'000'000LL));
    APW_EXPECT(Insert(missing, 22U, 2'704U, 2'016'000'000LL));
    APW_EXPECT(missing.Pop(output) == JitterPopKind::Packet);
    APW_EXPECT(output.sequence_number == 20U);
    APW_EXPECT(missing.Pop(output) == JitterPopKind::Missing);
    APW_EXPECT(output.sequence_number == 21U);
    APW_EXPECT(missing.Pop(output) == JitterPopKind::Packet);
    APW_EXPECT(output.sequence_number == 22U);
    APW_EXPECT(missing.Diagnostics().lost_packets == 1U);
    APW_EXPECT(!Insert(missing, 21U, 2'352U, 2'020'000'000LL));
    APW_EXPECT(missing.Diagnostics().late_packets == 1U);

    RtpJitterBuffer wrapped{RtpJitterBufferConfig{
        .capacity_packets = 8U, .target_packets = 2U, .clock_rate = 44'100U}};
    APW_EXPECT(Insert(wrapped, 65'535U, 100U, 3'000'000'000LL));
    APW_EXPECT(Insert(wrapped, 0U, 452U, 3'008'000'000LL));
    APW_EXPECT(wrapped.Pop(output) == JitterPopKind::Packet);
    APW_EXPECT(output.extended_sequence_number == 65'535U);
    APW_EXPECT(wrapped.Pop(output) == JitterPopKind::Packet);
    APW_EXPECT(output.extended_sequence_number == 65'536U);
    wrapped.Flush();
    const auto flushed = wrapped.Diagnostics();
    APW_EXPECT(flushed.buffered_packets == 0U);
    APW_EXPECT(flushed.flush_count == 1U);
}
