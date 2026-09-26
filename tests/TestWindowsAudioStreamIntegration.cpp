#include "TestFramework.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

#include "RtpTestUtils.h"
#include "core/audio/IAudioOutput.h"
#include "core/audio/PcmL16Decoder.h"
#include "core/transport/RtpAudioStream.h"
#include "platform/windows/audio/WindowsAudioStreamSink.h"
#include "platform/windows/timing/QpcClock.h"

namespace {

class InspectableAudioOutput final : public airplaywin::audio::IAudioOutput {
public:
    bool Open(const airplaywin::audio::AudioFormat& format) override {
        sample_rate_.store(format.sample_rate, std::memory_order_release);
        opened_.store(format.IsValid(), std::memory_order_release);
        return format.IsValid();
    }

    bool Start() override {
        if (!opened_.load(std::memory_order_acquire)) {
            return false;
        }
        started_.store(true, std::memory_order_release);
        start_count_.fetch_add(1U, std::memory_order_relaxed);
        return true;
    }

    bool Write(const airplaywin::audio::AudioBuffer& buffer) override {
        if (!opened_.load(std::memory_order_acquire) || !buffer.IsValid() ||
            buffer.epoch_id != epoch_.load(std::memory_order_acquire)) {
            return false;
        }
        last_epoch_.store(buffer.epoch_id, std::memory_order_release);
        last_timestamp_qpc_.store(buffer.timestamp_qpc, std::memory_order_release);
        frames_.fetch_add(buffer.frame_count, std::memory_order_relaxed);
        writes_.fetch_add(1U, std::memory_order_relaxed);
        return true;
    }

    void Pause() noexcept override {
        pause_count_.fetch_add(1U, std::memory_order_relaxed);
    }

    void Resume() noexcept override {
        resume_count_.fetch_add(1U, std::memory_order_relaxed);
    }

    void Stop() noexcept override {
        started_.store(false, std::memory_order_release);
        stop_count_.fetch_add(1U, std::memory_order_relaxed);
    }

    void Flush() noexcept override {}

    void BeginEpoch(const std::uint64_t epoch_id,
                    const airplaywin::audio::AudioTransition transition) noexcept override {
        epoch_.store(epoch_id, std::memory_order_release);
        transition_.store(transition, std::memory_order_release);
    }

    void SetVolume(const float linear_gain) noexcept override {
        volume_.store(linear_gain, std::memory_order_release);
    }

    void Close() noexcept override {
        opened_.store(false, std::memory_order_release);
        close_count_.fetch_add(1U, std::memory_order_relaxed);
    }

    [[nodiscard]] airplaywin::audio::AudioDiagnosticsSnapshot Diagnostics() const override {
        return {.rendered_frames = frames_.load(std::memory_order_relaxed),
                .current_sample_rate = sample_rate_.load(std::memory_order_acquire),
                .current_audio_epoch = epoch_.load(std::memory_order_acquire)};
    }

    std::atomic<std::uint64_t> epoch_{0U};
    std::atomic<std::uint64_t> last_epoch_{0U};
    std::atomic<std::int64_t> last_timestamp_qpc_{0};
    std::atomic<std::uint64_t> writes_{0U};
    std::atomic<std::uint64_t> frames_{0U};
    std::atomic<std::uint64_t> start_count_{0U};
    std::atomic<std::uint64_t> pause_count_{0U};
    std::atomic<std::uint64_t> resume_count_{0U};
    std::atomic<std::uint64_t> stop_count_{0U};
    std::atomic<std::uint64_t> close_count_{0U};
    std::atomic<std::uint32_t> sample_rate_{0U};
    std::atomic<float> volume_{1.0F};
    std::atomic<airplaywin::audio::AudioTransition> transition_{
        airplaywin::audio::AudioTransition::None};
    std::atomic<bool> opened_{false};
    std::atomic<bool> started_{false};
};

template <typename Predicate>
[[nodiscard]] bool WaitUntil(Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return predicate();
}

}  // namespace

void TestWindowsAudioStreamIntegration() {
    using airplaywin::audio::AudioTransition;
    using airplaywin::tests::BuildRtpPacket;
    using airplaywin::transport::DatagramEndpoint;
    using airplaywin::transport::RtpAudioStream;
    using airplaywin::transport::RtpAudioStreamConfig;

    auto output = std::make_unique<InspectableAudioOutput>();
    auto* const observed_output = output.get();
    airplaywin::windows::audio::WindowsAudioStreamSink sink{std::move(output), 5U};
    RtpAudioStream stream{
        RtpAudioStreamConfig{
            .connection_id = 700U,
            .format = {.sample_rate = 8'000U,
                       .channel_count = 1U,
                       .payload_type = 96U,
                       .nominal_frames_per_packet = 40U},
            .jitter_buffer =
                {.capacity_packets = 8U, .target_packets = 1U, .clock_rate = 8'000U},
        },
        std::make_unique<airplaywin::audio::PcmL16Decoder>(), sink};
    APW_EXPECT(stream.Start());
    APW_EXPECT(stream.Record({.sequence_number = static_cast<std::uint16_t>(1U),
                              .rtp_timestamp = 1'000U}));

    std::array<std::byte, 80U> payload{};
    for (std::size_t index = 0U; index < payload.size(); index += 2U) {
        payload[index] = std::byte{0x10U};
    }
    const DatagramEndpoint source{.ipv4_address_network_order = 0x0100'007FU,
                                  .port = 7'000U};
    const auto first = BuildRtpPacket(1U, 1'000U, payload);
    stream.OnDatagram(first, source, 1'000'000'000LL);
    APW_EXPECT(WaitUntil([&] {
        return observed_output->writes_.load(std::memory_order_acquire) == 1U &&
               observed_output->start_count_.load(std::memory_order_acquire) == 1U;
    }));
    APW_EXPECT(observed_output->last_epoch_.load(std::memory_order_acquire) == 1U);
    APW_EXPECT(observed_output->last_timestamp_qpc_.load(std::memory_order_acquire) > 0);

    stream.SetVolume(0.25F);
    APW_EXPECT_NEAR(observed_output->volume_.load(std::memory_order_acquire), 0.25, 0.00001);
    stream.Pause();
    stream.Resume({});
    APW_EXPECT(observed_output->pause_count_.load(std::memory_order_acquire) == 1U);
    APW_EXPECT(observed_output->resume_count_.load(std::memory_order_acquire) == 1U);

    stream.Flush({.sequence_number = static_cast<std::uint16_t>(10U),
                  .rtp_timestamp = 2'000U});
    APW_EXPECT(observed_output->epoch_.load(std::memory_order_acquire) == 2U);
    APW_EXPECT(observed_output->transition_.load(std::memory_order_acquire) ==
               AudioTransition::Flush);
    const auto stale = BuildRtpPacket(9U, 1'999U, payload);
    stream.OnDatagram(stale, source, 1'010'000'000LL);
    const auto current = BuildRtpPacket(10U, 2'000U, payload);
    stream.OnDatagram(current, source, 1'011'000'000LL);
    APW_EXPECT(WaitUntil([&] {
        return observed_output->writes_.load(std::memory_order_acquire) == 2U;
    }));
    APW_EXPECT(observed_output->last_epoch_.load(std::memory_order_acquire) == 2U);
    APW_EXPECT(stream.Diagnostics().timeline_rejected_packets == 1U);

    stream.Stop();
    APW_EXPECT(observed_output->stop_count_.load(std::memory_order_acquire) == 1U);
    APW_EXPECT(observed_output->close_count_.load(std::memory_order_acquire) == 1U);

    auto scheduled_output = std::make_unique<InspectableAudioOutput>();
    auto* const observed_scheduled_output = scheduled_output.get();
    airplaywin::windows::audio::WindowsAudioStreamSink scheduled_sink{
        std::move(scheduled_output), 5U, 5U};
    APW_EXPECT(scheduled_sink.Configure(
        {.sample_rate = 8'000U, .channel_count = 1U}));
    APW_EXPECT(scheduled_sink.Start());
    std::array<float, 40U> scheduled_samples{};
    const auto frequency = airplaywin::windows::timing::QpcClock::Frequency();
    APW_EXPECT(frequency > 0);
    const auto target = airplaywin::windows::timing::QpcClock::Now() + frequency * 30 / 1'000;
    const auto wait_started = std::chrono::steady_clock::now();
    APW_EXPECT(scheduled_sink.Submit({
        .interleaved_samples = scheduled_samples,
        .frame_count = static_cast<std::uint32_t>(scheduled_samples.size()),
        .rtp_timestamp = 3'000U,
        .extended_sequence_number = 30U,
        .target_qpc = target,
    }));
    const auto waited = std::chrono::steady_clock::now() - wait_started;
    APW_EXPECT(waited >= std::chrono::milliseconds{15});
    const auto scheduled = scheduled_sink.Diagnostics();
    APW_EXPECT(scheduled.scheduled_frames == scheduled_samples.size());
    APW_EXPECT(scheduled.scheduling_wait_microseconds >= 15'000U);
    APW_EXPECT(scheduled.last_target_qpc == target);
    APW_EXPECT(observed_scheduled_output->last_timestamp_qpc_.load(
                   std::memory_order_acquire) == target);
    const auto epoch_before_resync =
        observed_scheduled_output->epoch_.load(std::memory_order_acquire);
    scheduled_sink.HardResync();
    APW_EXPECT(observed_scheduled_output->epoch_.load(std::memory_order_acquire) ==
               epoch_before_resync + 1U);
    APW_EXPECT(observed_scheduled_output->transition_.load(std::memory_order_acquire) ==
               AudioTransition::HardResync);

    const auto cancelled_target = airplaywin::windows::timing::QpcClock::Now() +
                                  frequency * 250 / 1'000;
    std::atomic<bool> cancelled_submission{true};
    const auto cancellation_started = std::chrono::steady_clock::now();
    std::thread pending_submitter{[&] {
        cancelled_submission.store(
            scheduled_sink.Submit({
                .interleaved_samples = scheduled_samples,
                .frame_count = static_cast<std::uint32_t>(scheduled_samples.size()),
                .rtp_timestamp = 3'040U,
                .extended_sequence_number = 31U,
                .target_qpc = cancelled_target,
            }),
            std::memory_order_release);
    }};
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    scheduled_sink.Flush();
    pending_submitter.join();
    APW_EXPECT(!cancelled_submission.load(std::memory_order_acquire));
    APW_EXPECT(std::chrono::steady_clock::now() - cancellation_started <
               std::chrono::milliseconds{200});
    APW_EXPECT(scheduled_sink.Diagnostics().rejected_frames >=
               scheduled_samples.size());
    scheduled_sink.Stop();
}
