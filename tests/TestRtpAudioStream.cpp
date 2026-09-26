#include "TestFramework.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>

#include "FakeAudioFrameSink.h"
#include "RtpTestUtils.h"
#include "core/audio/PcmL16Decoder.h"
#include "core/timing/ITimingEngine.h"
#include "core/transport/RtpAudioStream.h"

namespace {

class ServoTimingEngine final : public airplaywin::timing::ITimingEngine {
public:
    void Reset(const std::optional<std::uint64_t> remote_time) noexcept override {
        anchor_.store(remote_time.value_or(0U), std::memory_order_release);
    }

    [[nodiscard]] std::optional<std::int64_t> RemoteToLocalQpc(
        const std::uint64_t remote_time) noexcept override {
        return static_cast<std::int64_t>(remote_time + 1'000U);
    }

    [[nodiscard]] airplaywin::timing::TimingDiagnostics Diagnostics()
        const noexcept override {
        airplaywin::timing::TimingDiagnostics result;
        result.enabled = true;
        result.locked = true;
        result.servo.locked = true;
        result.servo.rate_correction = 1.01;
        return result;
    }

    [[nodiscard]] double RateCorrection() const noexcept override { return 1.01; }

    [[nodiscard]] bool ConsumeHardResyncRequest() noexcept override {
        return hard_resync_.exchange(false, std::memory_order_acq_rel);
    }

private:
    std::atomic<std::uint64_t> anchor_{0U};
    std::atomic<bool> hard_resync_{true};
};

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
    sink.SetOutputFeedback(240U, 7'000U);
    RtpAudioStream stream{
        RtpAudioStreamConfig{
            .connection_id = 42U,
            .format = {.sample_rate = 44'100U,
                       .channel_count = 2U,
                       .payload_type = 96U,
                       .nominal_frames_per_packet = 1U},
            .jitter_buffer = {.capacity_packets = 8U,
                              .target_packets = 2U,
                              .clock_rate = 44'100U,
                              .adaptive_enabled = true,
                              .minimum_target_packets = 1U,
                              .maximum_target_packets = 6U,
                              .stable_window_packets = 8U,
                              .recovery_window_packets = 4U},
            .protocol_latency_frames = 11'025U,
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
    APW_EXPECT(diagnostics.protocol_latency_frames == 11'025U);
    APW_EXPECT(diagnostics.protocol_latency_microseconds == 250'000U);
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
    const auto latency_diagnostics = stream.Diagnostics();
    APW_EXPECT(latency_diagnostics.packet_processing_average_microseconds >= 1U);
    APW_EXPECT(latency_diagnostics.packet_processing_maximum_microseconds >=
               latency_diagnostics.packet_processing_average_microseconds);
    APW_EXPECT(latency_diagnostics.decode_processing_average_microseconds >= 1U);
    APW_EXPECT(latency_diagnostics.decode_processing_maximum_microseconds >=
               latency_diagnostics.decode_processing_average_microseconds);
    APW_EXPECT(latency_diagnostics.output_path_latency_microseconds == 7'000U);
    APW_EXPECT(latency_diagnostics.receiver_added_latency_estimate_microseconds >= 7'000U);
    sink.SetUnderrunCount(3U);
    const auto feedback_packet = BuildRtpPacket(2U, 2U, positive);
    stream.OnDatagram(feedback_packet, source, 1'008'000'000LL);
    const auto feedback_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (stream.Diagnostics().jitter_buffer.downstream_underruns != 3U &&
           std::chrono::steady_clock::now() < feedback_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    APW_EXPECT(stream.Diagnostics().jitter_buffer.downstream_underruns == 3U);
    APW_EXPECT(stream.Diagnostics().jitter_buffer.adaptive_state ==
               airplaywin::transport::AdaptiveJitterState::Degraded);
    stream.Stop();
    APW_EXPECT(sink.StopCount() == 1U);

    FakeAudioFrameSink disciplined_sink;
    RtpAudioStream disciplined_stream{
        RtpAudioStreamConfig{
            .connection_id = 43U,
            .format = {.sample_rate = 8'000U,
                       .channel_count = 1U,
                       .payload_type = 96U,
                       .nominal_frames_per_packet = 100U},
            .jitter_buffer = {.capacity_packets = 16U,
                              .target_packets = 1U,
                              .clock_rate = 8'000U},
        },
        std::make_unique<airplaywin::audio::PcmL16Decoder>(), disciplined_sink,
        std::make_unique<ServoTimingEngine>()};
    APW_EXPECT(disciplined_stream.Start());
    APW_EXPECT(disciplined_stream.Record(
        {.sequence_number = static_cast<std::uint16_t>(1U), .rtp_timestamp = 1'000U}));
    std::array<std::byte, 200U> servo_payload{};
    for (std::uint16_t packet = 0U; packet < 10U; ++packet) {
        disciplined_stream.OnDatagram(
            BuildRtpPacket(static_cast<std::uint16_t>(packet + 1U),
                           1'000U + static_cast<std::uint32_t>(packet) * 100U,
                           servo_payload),
            source, 2'000'000'000LL + static_cast<std::int64_t>(packet) * 12'500'000LL);
    }
    APW_EXPECT(WaitForFrames(disciplined_sink, 1'010U));
    const auto disciplined = disciplined_stream.Diagnostics();
    APW_EXPECT(disciplined.resampled_input_frames == 1'000U);
    APW_EXPECT(disciplined.resampled_output_frames == 1'010U);
    APW_EXPECT(disciplined.drift_inserted_frames == 10U);
    APW_EXPECT(disciplined.drift_dropped_frames == 0U);
    APW_EXPECT(disciplined.timing_hard_resync_requests == 1U);
    APW_EXPECT(disciplined_sink.HardResyncCount() == 1U);
    disciplined_stream.Stop();
}
