#pragma once

#include "core/audio/AudioStreamTypes.h"

namespace airplaywin::audio {

class IAudioFrameSink {
public:
    virtual ~IAudioFrameSink() = default;

    [[nodiscard]] virtual bool Configure(const AudioFormat& format) = 0;
    [[nodiscard]] virtual bool Start() = 0;
    [[nodiscard]] virtual bool Submit(const DecodedAudioFrameView& frame) noexcept = 0;
    virtual void Pause() noexcept = 0;
    virtual void Resume() noexcept = 0;
    virtual void Flush() noexcept = 0;
    virtual void SetVolume(float linear_gain) noexcept = 0;
    virtual void Stop() noexcept = 0;
    [[nodiscard]] virtual AudioSinkFeedback Feedback() const noexcept { return {}; }
};

}  // namespace airplaywin::audio
