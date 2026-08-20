#include "TestFramework.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>

#include "FakeAudioFrameSink.h"
#include "RtpTestUtils.h"
#include "core/audio/PcmL16Decoder.h"
#include "core/timing/BufferedRtpTimingEngine.h"
#include "core/transport/RtpAudioStream.h"

namespace {

class ManualClock final : public airplaywin::timing::IMonotonicClock {
public:
    ManualClock(const std::int64_t now, const std::int64_t frequency) noexcept
        : now_(now), frequency_(frequency) {}

    [[nodiscard]] std::int64_t Now() const noexcept override {
        return now_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::int64_t Frequency() const noexcept override {
        return frequency_;
    }

    void Set(const std::int64_t now) noexcept {
        now_.store(now, std::memory_order_release);
    }

private:
    std::atomic<std::int64_t> now_{0};
    std::int64_t frequency_{0};
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

void TestBufferedTiming() {
    using airplaywin::timing::BufferedRtpTimingConfig;
    using airplaywin::timing::BufferedRtpTimingEngine;

    auto clock = std::make_unique<ManualClock>(1'000'000LL, 1'000'000LL);
    auto* const observed_clock = clock.get();
    BufferedRtpTimingEngine model{
        BufferedRtpTimingConfig{.remote_clock_rate = 8'000U,
                                .target_buffer_milliseconds = 100U},
        std::move(clock)};
    model.Reset(0xFFFF'FFF0U);
    APW_EXPECT(model.RemoteToLocalQpc(0xFFFF'FFF0U) == 1'100'000LL);
    APW_EXPECT(model.RemoteToLocalQpc(0x0000'0010U) == 1'104'000LL);
    observed_clock->Set(1'200'000LL);
    APW_EXPECT(model.RemoteToLocalQpc(0x0000'0020U) == 1'106'000LL);
    auto diagnostics = model.Diagnostics();
    APW_EXPECT(diagnostics.enabled && diagnostics.locked);
    APW_EXPECT(diagnostics.generation == 1U);
    APW_EXPECT(diagnostics.mapped_packets == 3U);
    APW_EXPECT(diagnostics.late_mappings == 1U);
    APW_EXPECT(diagnostics.maximum_lateness_microseconds == 94'000U);

    observed_clock->Set(2'000'000LL);
    model.Reset(std::nullopt);
    APW_EXPECT(!model.Diagnostics().locked);
    APW_EXPECT(model.RemoteToLocalQpc(500U) == 2'100'000LL);
    APW_EXPECT(model.Diagnostics().generation == 2U);

    bool rejected_invalid_clock = false;
    try {
        static_cast<void>(BufferedRtpTimingEngine{
            BufferedRtpTimingConfig{}, std::make_unique<ManualClock>(0LL, 0LL)});
    } catch (const std::invalid_argument&) {
        rejected_invalid_clock = true;
    }
    APW_EXPECT(rejected_invalid_clock);

    auto drift_clock = std::make_unique<ManualClock>(0LL, 10'000'000LL);
    auto* const observed_drift_clock = drift_clock.get();
    BufferedRtpTimingEngine drift_model{
        BufferedRtpTimingConfig{.remote_clock_rate = 48'000U,
                                .target_buffer_milliseconds = 100U},
        std::move(drift_clock)};
    drift_model.Reset(0U);
    for (std::uint64_t second = 0U; second <= 1'100U; ++second) {
        // A local clock running 100 ppm faster accumulates 10 ms of error after
        // the initial 100 ms reserve is exhausted over 1,100 simulated seconds.
        observed_drift_clock->Set(static_cast<std::int64_t>(second * 10'001'000U));
        APW_EXPECT(drift_model.RemoteToLocalQpc(second * 48'000U).has_value());
    }
    const auto drift = drift_model.Diagnostics();
    APW_EXPECT(drift.mapped_packets == 1'101U);
    APW_EXPECT(drift.maximum_lateness_microseconds == 10'000U);

    auto stream_clock = std::make_unique<ManualClock>(10'000'000LL, 1'000'000LL);
    auto* const observed_stream_clock = stream_clock.get();
    auto timing = std::make_unique<BufferedRtpTimingEngine>(
        BufferedRtpTimingConfig{.remote_clock_rate = 8'000U,
                                .target_buffer_milliseconds = 50U},
        std::move(stream_clock));
    airplaywin::tests::FakeAudioFrameSink sink;
    airplaywin::transport::RtpAudioStream stream{
        airplaywin::transport::RtpAudioStreamConfig{
            .connection_id = 77U,
            .format = {.sample_rate = 8'000U,
                       .channel_count = 1U,
                       .payload_type = 96U,
                       .nominal_frames_per_packet = 40U},
            .jitter_buffer = {.capacity_packets = 8U,
                              .target_packets = 1U,
                              .clock_rate = 8'000U},
        },
        std::make_unique<airplaywin::audio::PcmL16Decoder>(), sink, std::move(timing)};
    APW_EXPECT(stream.Start());
    APW_EXPECT(stream.Record({.sequence_number = static_cast<std::uint16_t>(1U),
                              .rtp_timestamp = 1'000U}));

    std::array<std::byte, 80U> payload{};
    const airplaywin::transport::DatagramEndpoint source{
        .ipv4_address_network_order = 0x0100'007FU, .port = 7'000U};
    stream.OnDatagram(airplaywin::tests::BuildRtpPacket(1U, 1'000U, payload), source,
                      1'000'000'000LL);
    observed_stream_clock->Set(10'010'000LL);
    stream.OnDatagram(airplaywin::tests::BuildRtpPacket(2U, 1'040U, payload), source,
                      1'005'000'000LL);
    APW_EXPECT(WaitForFrames(sink, 80U));
    const auto targets = sink.Targets();
    APW_EXPECT(targets.size() == 2U);
    APW_EXPECT(targets[0] == 10'050'000LL);
    APW_EXPECT(targets[1] == 10'055'000LL);

    stream.Pause();
    observed_stream_clock->Set(15'000'000LL);
    stream.Resume({});
    stream.OnDatagram(airplaywin::tests::BuildRtpPacket(3U, 1'080U, payload), source,
                      1'010'000'000LL);
    APW_EXPECT(WaitForFrames(sink, 120U));
    const auto resumed_targets = sink.Targets();
    APW_EXPECT(resumed_targets.back() == 15'050'000LL);

    observed_stream_clock->Set(20'000'000LL);
    stream.Flush({.sequence_number = static_cast<std::uint16_t>(10U),
                  .rtp_timestamp = 2'000U});
    stream.OnDatagram(airplaywin::tests::BuildRtpPacket(10U, 2'000U, payload), source,
                      2'000'000'000LL);
    APW_EXPECT(WaitForFrames(sink, 160U));
    const auto reset_targets = sink.Targets();
    APW_EXPECT(reset_targets.back() == 20'050'000LL);
    diagnostics = stream.Diagnostics().timing;
    APW_EXPECT(diagnostics.generation == 3U);
    APW_EXPECT(diagnostics.mapped_packets == 4U);
    stream.Stop();
}
