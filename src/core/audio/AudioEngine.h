#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>

#include "core/audio/AudioEpoch.h"
#include "core/audio/IAudioOutput.h"

namespace airplaywin::audio {

// Protocol/session code submits PCM only through this facade. The facade owns
// the timeline epoch and the platform output enforces the final transition gate.
class AudioEngine final {
public:
    explicit AudioEngine(std::unique_ptr<IAudioOutput> output);

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    [[nodiscard]] bool Open(const AudioFormat& format);
    [[nodiscard]] bool Start();
    [[nodiscard]] bool Submit(std::span<const float> interleaved_samples,
                              std::uint32_t frame_count,
                              std::int64_t timestamp_qpc) noexcept;
    [[nodiscard]] bool Submit(const AudioBuffer& buffer) noexcept;
    void Pause() noexcept;
    void Resume() noexcept;
    void Stop() noexcept;
    void Flush() noexcept;
    void Seek() noexcept;
    void HardResync() noexcept;
    void ReplaceSender() noexcept;
    void ResetSession() noexcept;
    [[nodiscard]] bool ChangeFormat(const AudioFormat& format);
    void SetVolume(float linear_gain) noexcept;
    void Close() noexcept;

    [[nodiscard]] std::uint64_t CurrentEpoch() const noexcept;
    [[nodiscard]] AudioDiagnosticsSnapshot Diagnostics() const;

private:
    void AdvanceEpoch(AudioEpochReason reason, AudioTransition transition) noexcept;

    std::unique_ptr<IAudioOutput> output_;
    AudioEpoch epoch_{};
    std::atomic<std::uint32_t> sample_rate_{0U};
    std::atomic<std::uint16_t> channel_count_{0U};
    std::atomic<bool> is_open_{false};
    std::atomic<bool> is_started_{false};
};

}  // namespace airplaywin::audio
