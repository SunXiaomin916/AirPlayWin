#pragma once

#include <cstdint>

#include "core/audio/AudioTypes.h"

namespace airplaywin::audio {

class IAudioOutput {
public:
    virtual ~IAudioOutput() = default;

    virtual bool Open(const AudioFormat& format) = 0;
    virtual bool Start() = 0;
    virtual bool Write(const AudioBuffer& buffer) = 0;
    virtual void Pause() noexcept = 0;
    virtual void Resume() noexcept = 0;
    virtual void Stop() noexcept = 0;
    virtual void Flush() noexcept = 0;
    virtual void BeginEpoch(std::uint64_t epoch_id, AudioTransition transition) noexcept = 0;
    virtual void SetVolume(float linear_gain) noexcept = 0;
    virtual void Close() noexcept = 0;

    [[nodiscard]] virtual AudioDiagnosticsSnapshot Diagnostics() const = 0;
};

}  // namespace airplaywin::audio
