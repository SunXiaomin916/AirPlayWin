#include "TestFramework.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>

#include "FakeAudioFrameSink.h"
#include "RtpTestUtils.h"
#include "core/audio/PcmL16Decoder.h"
#include "core/transport/RtpAudioStream.h"

namespace {

[[nodiscard]] bool WaitForFrames(const airplaywin::tests::FakeAudioFrameSink& sink,
                                 const std::uint64_t frames) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        if (sink.SubmittedFrames() >= frames) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return sink.SubmittedFrames() >= frames;
}

}  // namespace

void TestRtpAudioStream() {
    using airplaywin::tests::BuildRtpPacket;
    using airplaywin::tests::FakeAudioFrameSink;
    using airplaywin::transport::DatagramEndpoint;
    using airplaywin::transport::RtpAudioStream;
    using airplaywin::transport::RtpAudioStreamConfig;

    FakeAudioFrameSink sink;
    RtpAudioStream stream{
        RtpAudioStreamConfig{
            .connection_id = 42U,
            .format = {.sample_rate = 44'100U,
                       .channel_count = 2U,
                       .payload_type = 96U,
                       .nominal_frames_per_packet = 1U},
            .jitter_buffer = {.capacity_packets = 8U,
                              .target_packets = 2U,
                              .clock_rate = 44'100U},
        },
        std::make_unique<airplaywin::audio::PcmL16Decoder>(), sink};
    APW_EXPECT(stream.Start());
    APW_EXPECT(stream.Record({.sequence_number = static_cast<std::uint16_t>(1U),
                              .rtp_timestamp = 1'000U}));

    const DatagramEndpoint source{.ipv4_address_network_order = 0x0100'007FU,
                                  .port = 7'000U};
    const std::array positive{
        std::byte{0x40U}, std::byte{0x00U}, std::byte{0xC0U}, std::byte{0x00U}};
    const auto packet1 = BuildRtpPacket(1U, 1'000U, positive);
    const auto packet3 = BuildRtpPacket(3U, 1'002U, positive);
    const auto packet4 = BuildRtpPacket(4U, 1'003U, positive);
    stream.OnDatagram(packet1, source, 1'000'000'000LL);
    stream.OnDatagram(packet3, source, 1'002'000'000LL);
    stream.OnDatagram(packet4, source, 1'003'000'000LL);
    APW_EXPECT(WaitForFrames(sink, 4U));

    const auto diagnostics = stream.Diagnostics();
    APW_EXPECT(diagnostics.decoded_packets == 3U);
    APW_EXPECT(diagnostics.decoded_frames == 3U);
    APW_EXPECT(diagnostics.concealed_packets == 1U);
    APW_EXPECT(diagnostics.concealed_frames == 1U);
    APW_EXPECT(diagnostics.jitter_buffer.lost_packets == 1U);
    const auto samples = sink.Samples();
    APW_EXPECT(samples.size() == 8U);
    APW_EXPECT_NEAR(samples[0], 0.5, 0.00001);
    APW_EXPECT_NEAR(samples[1], -0.5, 0.00001);
    APW_EXPECT(samples[2] == 0.0F && samples[3] == 0.0F);
    const auto concealed = sink.Concealed();
    APW_EXPECT(concealed.size() == 4U);
    APW_EXPECT(!concealed[0] && concealed[1] && !concealed[2] && !concealed[3]);

    const std::array<std::byte, 3U> invalid{};
    stream.OnDatagram(invalid, source, 1'004'000'000LL);
    const auto wrong_payload = BuildRtpPacket(5U, 1'004U, positive, 97U);
    stream.OnDatagram(wrong_payload, source, 1'004'000'000LL);
    APW_EXPECT(stream.Diagnostics().invalid_rtp_packets == 1U);
    APW_EXPECT(stream.Diagnostics().unexpected_payload_packets == 1U);

    stream.Pause();
    const auto packet5 = BuildRtpPacket(5U, 1'004U, positive);
    stream.OnDatagram(packet5, source, 1'004'000'000LL);
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    APW_EXPECT(sink.SubmittedFrames() == 4U);
    stream.Resume({});
    APW_EXPECT(WaitForFrames(sink, 5U));
    stream.SetVolume(0.25F);
    APW_EXPECT_NEAR(sink.Volume(), 0.25, 0.00001);
    stream.Flush({.sequence_number = static_cast<std::uint16_t>(20U),
                  .rtp_timestamp = 2'000U});
    APW_EXPECT(sink.FlushCount() == 1U);
    const auto stale_after_flush = BuildRtpPacket(19U, 1'999U, positive);
    stream.OnDatagram(stale_after_flush, source, 1'005'000'000LL);
    APW_EXPECT(stream.Diagnostics().timeline_rejected_packets == 1U);
    APW_EXPECT(stream.Diagnostics().sequence_anchor == 20U);
    APW_EXPECT(stream.Diagnostics().timestamp_anchor == 2'000U);
    stream.Flush({.sequence_number = static_cast<std::uint16_t>(65'535U),
                  .rtp_timestamp = 4'294'967'295U});
    const auto wrapped_sequence0 = BuildRtpPacket(0U, 0U, positive);
    const auto wrapped_sequence1 = BuildRtpPacket(1U, 1U, positive);
    stream.OnDatagram(wrapped_sequence0, source, 1'006'000'000LL);
    stream.OnDatagram(wrapped_sequence1, source, 1'007'000'000LL);
    APW_EXPECT(WaitForFrames(sink, 7U));
    APW_EXPECT(stream.Diagnostics().timeline_rejected_packets == 1U);
    stream.Stop();
    APW_EXPECT(sink.StopCount() == 1U);
}
