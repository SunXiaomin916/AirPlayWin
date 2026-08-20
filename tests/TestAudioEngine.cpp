#include "TestFramework.h"

#include <array>
#include <memory>

#include "core/audio/AudioEngine.h"

namespace {

class FakeAudioOutput final : public airplaywin::audio::IAudioOutput {
public:
    bool Open(const airplaywin::audio::AudioFormat& format) override {
        format_ = format;
        opened_ = format.IsValid();
        return opened_;
    }
    bool Start() override {
        started_ = opened_;
        return started_;
    }
    bool Write(const airplaywin::audio::AudioBuffer& buffer) override {
        last_buffer_ = buffer;
        ++writes_;
        return started_ && buffer.IsValid() && buffer.epoch_id == epoch_;
    }
    void Pause() noexcept override {}
    void Resume() noexcept override {}
    void Stop() noexcept override { started_ = false; }
    void Flush() noexcept override {}
    void BeginEpoch(const std::uint64_t epoch_id,
                    const airplaywin::audio::AudioTransition transition) noexcept override {
        epoch_ = epoch_id;
        last_transition_ = transition;
    }
    void SetVolume(const float linear_gain) noexcept override { volume_ = linear_gain; }
    void Close() noexcept override {
        opened_ = false;
        started_ = false;
    }
    [[nodiscard]] airplaywin::audio::AudioDiagnosticsSnapshot Diagnostics() const override {
        return airplaywin::audio::AudioDiagnosticsSnapshot{
            .current_sample_rate = format_.sample_rate,
            .current_audio_epoch = epoch_,
        };
    }

    airplaywin::audio::AudioFormat format_{};
    airplaywin::audio::AudioBuffer last_buffer_{};
    airplaywin::audio::AudioTransition last_transition_{
        airplaywin::audio::AudioTransition::None};
    std::uint64_t epoch_{0U};
    std::uint32_t writes_{0U};
    float volume_{1.0F};
    bool opened_{false};
    bool started_{false};
};

}  // namespace

void TestAudioEngine() {
    using airplaywin::audio::AudioEngine;
    using airplaywin::audio::AudioFormat;
    using airplaywin::audio::AudioTransition;

    auto fake = std::make_unique<FakeAudioOutput>();
    auto* const fake_observer = fake.get();
    AudioEngine engine{std::move(fake)};
    const AudioFormat format{.sample_rate = 48'000U, .channel_count = 2U};
    APW_EXPECT(engine.Open(format));
    APW_EXPECT(engine.Start());

    const std::array<float, 8U> samples{};
    APW_EXPECT(engine.Submit(samples, 4U, 1234));
    APW_EXPECT(fake_observer->last_buffer_.epoch_id == 1U);
    APW_EXPECT(fake_observer->last_buffer_.timestamp_qpc == 1234);
    APW_EXPECT(fake_observer->last_buffer_.frame_count == 4U);

    engine.Flush();
    APW_EXPECT(engine.CurrentEpoch() == 2U);
    APW_EXPECT(fake_observer->epoch_ == 2U);
    APW_EXPECT(fake_observer->last_transition_ == AudioTransition::Flush);
    const airplaywin::audio::AudioBuffer stale_buffer{
        .interleaved_samples = samples,
        .epoch_id = 1U,
        .timestamp_qpc = 2000,
        .frame_count = 4U,
        .sample_rate = 48'000U,
        .channel_count = 2U,
    };
    APW_EXPECT(!engine.Submit(stale_buffer));

    engine.HardResync();
    APW_EXPECT(engine.CurrentEpoch() == 3U);
    APW_EXPECT(fake_observer->last_transition_ == AudioTransition::HardResync);
    engine.SetVolume(0.25F);
    APW_EXPECT(fake_observer->volume_ == 0.25F);
    engine.Close();
}
