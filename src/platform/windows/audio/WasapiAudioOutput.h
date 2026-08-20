#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/audio/IAudioOutput.h"

namespace airplaywin::windows::audio {

struct WasapiOutputOptions final {
    std::wstring device_id{};
    bool follow_default_device{true};
    std::uint32_t ring_capacity_milliseconds{500U};
    std::uint32_t target_queue_milliseconds{30U};
};

class WasapiAudioOutput final : public airplaywin::audio::IAudioOutput {
public:
    explicit WasapiAudioOutput(WasapiOutputOptions options = {});
    ~WasapiAudioOutput() override;

    WasapiAudioOutput(const WasapiAudioOutput&) = delete;
    WasapiAudioOutput& operator=(const WasapiAudioOutput&) = delete;

    bool Open(const airplaywin::audio::AudioFormat& format) override;
    bool Start() override;
    bool Write(const airplaywin::audio::AudioBuffer& buffer) override;
    void Pause() noexcept override;
    void Resume() noexcept override;
    void Stop() noexcept override;
    void Flush() noexcept override;
    void BeginEpoch(std::uint64_t epoch_id,
                    airplaywin::audio::AudioTransition transition) noexcept override;
    void SetVolume(float linear_gain) noexcept override;
    void Close() noexcept override;
    [[nodiscard]] airplaywin::audio::AudioDiagnosticsSnapshot Diagnostics() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace airplaywin::windows::audio
