#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include "core/audio/IAudioFrameSink.h"
#include "platform/windows/audio/WindowsAudioEngine.h"

namespace airplaywin::windows::audio {

struct AudioStreamSinkDiagnostics final {
    bool configured{false};
    bool armed{false};
    bool started{false};
    std::uint64_t accepted_frames{0U};
    std::uint64_t rejected_frames{0U};
    airplaywin::audio::AudioDiagnosticsSnapshot output{};
};

class WindowsAudioStreamSink final : public airplaywin::audio::IAudioFrameSink {
public:
    explicit WindowsAudioStreamSink(WasapiOutputOptions options = {},
                                    std::uint32_t prefill_milliseconds = 20U);
    explicit WindowsAudioStreamSink(
        std::unique_ptr<airplaywin::audio::IAudioOutput> output,
        std::uint32_t prefill_milliseconds = 20U);

    [[nodiscard]] bool Configure(const airplaywin::audio::AudioFormat& format) override;
    [[nodiscard]] bool Start() override;
    [[nodiscard]] bool Submit(
        const airplaywin::audio::DecodedAudioFrameView& frame) noexcept override;
    void Pause() noexcept override;
    void Resume() noexcept override;
    void Flush() noexcept override;
    void SetVolume(float linear_gain) noexcept override;
    void Stop() noexcept override;

    [[nodiscard]] AudioStreamSinkDiagnostics Diagnostics() const;

private:
    mutable std::mutex mutex_{};
    WindowsAudioEngine engine_;
    std::uint32_t prefill_milliseconds_{20U};
    airplaywin::audio::AudioFormat format_{};
    std::uint64_t prefilled_frames_{0U};
    std::uint64_t accepted_frames_{0U};
    std::uint64_t rejected_frames_{0U};
    bool configured_{false};
    bool armed_{false};
    bool started_{false};
    bool resume_after_prefill_{false};
};

}  // namespace airplaywin::windows::audio
