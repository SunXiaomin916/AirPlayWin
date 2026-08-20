#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "core/audio/AudioEngine.h"
#include "platform/windows/audio/WasapiAudioOutput.h"

namespace airplaywin::windows::audio {

// Windows composition root for the platform-neutral engine and WASAPI backend.
class WindowsAudioEngine final {
public:
    explicit WindowsAudioEngine(WasapiOutputOptions options = {});
    explicit WindowsAudioEngine(
        std::unique_ptr<airplaywin::audio::IAudioOutput> output);

    [[nodiscard]] bool Open(const airplaywin::audio::AudioFormat& format);
    [[nodiscard]] bool Start();
    [[nodiscard]] bool Submit(std::span<const float> interleaved_samples,
                              std::uint32_t frame_count,
                              std::int64_t timestamp_qpc) noexcept;
    [[nodiscard]] bool Submit(const airplaywin::audio::AudioBuffer& buffer) noexcept;
    void Pause() noexcept;
    void Resume() noexcept;
    void Stop() noexcept;
    void Flush() noexcept;
    void Seek() noexcept;
    void HardResync() noexcept;
    void ReplaceSender() noexcept;
    void ResetSession() noexcept;
    [[nodiscard]] bool ChangeFormat(const airplaywin::audio::AudioFormat& format);
    void SetVolume(float linear_gain) noexcept;
    void Close() noexcept;

    [[nodiscard]] std::uint64_t CurrentEpoch() const noexcept;
    [[nodiscard]] airplaywin::audio::AudioDiagnosticsSnapshot Diagnostics() const;

private:
    airplaywin::audio::AudioEngine engine_;
};

}  // namespace airplaywin::windows::audio
