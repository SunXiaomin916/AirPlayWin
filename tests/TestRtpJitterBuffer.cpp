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
    APW_EXPECT(reordered.Pop(output, 2'000'000'000LL) == JitterPopKind::Packet);
    APW_EXPECT(output.sequence_number == 10U);
    APW_EXPECT(reordered.Pop(output, 2'000'000'000LL) == JitterPopKind::Packet);
    APW_EXPECT(output.sequence_number == 11U);
    APW_EXPECT(reordered.Pop(output, 2'000'000'000LL) == JitterPopKind::Packet);
    APW_EXPECT(output.sequence_number == 12U);
    APW_EXPECT(reordered.Pop(output, 2'000'000'000LL) == JitterPopKind::Waiting);
    const auto reorder_diagnostics = reordered.Diagnostics();
    APW_EXPECT(reorder_diagnostics.reordered_packets == 1U);
    APW_EXPECT(reorder_diagnostics.duplicate_packets == 1U);
    APW_EXPECT(reorder_diagnostics.emitted_packets == 3U);

    RtpJitterBuffer missing{RtpJitterBufferConfig{
        .capacity_packets = 8U,
        .target_packets = 2U,
        .clock_rate = 44'100U,
        .resend_wait_milliseconds = 0U}};
    APW_EXPECT(Insert(missing, 20U, 2'000U, 2'000'000'000LL));
    APW_EXPECT(Insert(missing, 22U, 2'704U, 2'016'000'000LL));
    APW_EXPECT(Insert(missing, 23U, 3'056U, 2'024'000'000LL));
    APW_EXPECT(missing.Pop(output, 2'024'000'000LL) == JitterPopKind::Packet);
    APW_EXPECT(output.sequence_number == 20U);
    APW_EXPECT(missing.Pop(output, 2'024'000'000LL) == JitterPopKind::Missing);
    APW_EXPECT(output.sequence_number == 21U);
    APW_EXPECT(missing.Pop(output, 2'024'000'000LL) == JitterPopKind::Packet);
    APW_EXPECT(output.sequence_number == 22U);
    APW_EXPECT(missing.Pop(output, 2'024'000'000LL) == JitterPopKind::Packet);
    APW_EXPECT(output.sequence_number == 23U);
    APW_EXPECT(missing.Diagnostics().lost_packets == 1U);
    APW_EXPECT(!Insert(missing, 21U, 2'352U, 2'020'000'000LL));
    APW_EXPECT(missing.Diagnostics().late_packets == 1U);

    APW_EXPECT(Insert(missing, 25U, 3'760U, 2'040'000'000LL));
    APW_EXPECT(missing.Pop(output, 2'040'000'000LL) == JitterPopKind::Waiting);
    APW_EXPECT(Insert(missing, 24U, 3'408U, 2'032'000'000LL));
    APW_EXPECT(missing.Pop(output, 2'040'000'000LL) == JitterPopKind::Packet);
    APW_EXPECT(output.sequence_number == 24U);
    APW_EXPECT(missing.Pop(output, 2'040'000'000LL) == JitterPopKind::Packet);
    APW_EXPECT(output.sequence_number == 25U);

    // Gap-hold: the loss gap opens, a retransmission request is queued, and
    // the retransmitted copy arriving inside the wait window is replayed in
    // place with no loss and no concealment.
    RtpJitterBuffer held{RtpJitterBufferConfig{
        .capacity_packets = 8U,
        .target_packets = 2U,
        .clock_rate = 44'100U,
        .reorder_depth_packets = 2U,
        .resend_wait_milliseconds = 50U}};
    APW_EXPECT(Insert(held, 30U, 3'000U, 4'000'000'000LL));
    APW_EXPECT(Insert(held, 32U, 3'704U, 4'016'000'000LL));
    APW_EXPECT(Insert(held, 33U, 4'056U, 4'024'000'000LL));
    APW_EXPECT(held.Pop(output, 4'024'000'000LL) == JitterPopKind::Packet);
    APW_EXPECT(output.sequence_number == 30U);
    APW_EXPECT(held.Pop(output, 4'024'000'000LL + 10'000'000LL) ==
               JitterPopKind::Waiting);
    std::uint16_t request_first = 0U;
    std::uint16_t request_count = 0U;
    APW_EXPECT(held.TakePendingResendRequest(request_first, request_count));
    APW_EXPECT(request_first == 31U);
    APW_EXPECT(request_count == 1U);
    APW_EXPECT(!held.TakePendingResendRequest(request_first, request_count));
    APW_EXPECT(Insert(held, 31U, 3'352U, 4'024'000'000LL + 20'000'000LL));
    APW_EXPECT(held.Pop(output, 4'024'000'000LL + 30'000'000LL) == JitterPopKind::Packet);
    APW_EXPECT(output.sequence_number == 31U);
    APW_EXPECT(held.Pop(output, 4'024'000'000LL + 30'000'000LL) == JitterPopKind::Packet);
    APW_EXPECT(output.sequence_number == 32U);
    APW_EXPECT(held.Pop(output, 4'024'000'000LL + 30'000'000LL) == JitterPopKind::Packet);
    APW_EXPECT(output.sequence_number == 33U);
    APW_EXPECT(held.Diagnostics().lost_packets == 0U);

    // Deadline expiry: the retransmission never arrives, so the packet is
    // declared lost and the buffered run continues without further holds.
    RtpJitterBuffer expired{RtpJitterBufferConfig{
        .capacity_packets = 8U,
        .target_packets = 2U,
        .clock_rate = 44'100U,
        .reorder_depth_packets = 2U,
        .resend_wait_milliseconds = 50U}};
    APW_EXPECT(Insert(expired, 40U, 4'000U, 5'000'000'000LL));
    APW_EXPECT(Insert(expired, 42U, 4'704U, 5'016'000'000LL));
    APW_EXPECT(Insert(expired, 43U, 5'056U, 5'024'000'000LL));
    APW_EXPECT(expired.Pop(output, 5'024'000'000LL) == JitterPopKind::Packet);
    APW_EXPECT(output.sequence_number == 40U);
    // The gap opens on this pop and starts its 50 ms hold window.
    APW_EXPECT(expired.Pop(output, 5'024'000'000LL + 100'000'000LL) ==
               JitterPopKind::Waiting);
    // Well past the deadline the packet is declared lost.
    APW_EXPECT(expired.Pop(output, 5'024'000'000LL + 200'000'000LL) ==
               JitterPopKind::Missing);
    APW_EXPECT(output.sequence_number == 41U);
    APW_EXPECT(expired.Pop(output, 5'024'000'000LL + 200'000'000LL) ==
               JitterPopKind::Packet);
    APW_EXPECT(output.sequence_number == 42U);
    APW_EXPECT(expired.Pop(output, 5'024'000'000LL + 200'000'000LL) ==
               JitterPopKind::Packet);
    APW_EXPECT(output.sequence_number == 43U);
    APW_EXPECT(expired.Diagnostics().lost_packets == 1U);

    RtpJitterBuffer wrapped{RtpJitterBufferConfig{
        .capacity_packets = 8U, .target_packets = 2U, .clock_rate = 44'100U}};
    APW_EXPECT(Insert(wrapped, 65'535U, 100U, 3'000'000'000LL));
    APW_EXPECT(Insert(wrapped, 0U, 452U, 3'008'000'000LL));
    APW_EXPECT(wrapped.Pop(output, 4'000'000'000LL) == JitterPopKind::Packet);
    APW_EXPECT(output.extended_sequence_number == 65'535U);
    APW_EXPECT(wrapped.Pop(output, 4'000'000'000LL) == JitterPopKind::Packet);
    APW_EXPECT(output.extended_sequence_number == 65'536U);
    wrapped.Flush();
    const auto flushed = wrapped.Diagnostics();
    APW_EXPECT(flushed.buffered_packets == 0U);
    APW_EXPECT(flushed.flush_count == 1U);
}
