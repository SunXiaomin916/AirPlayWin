#pragma once

#include <cstdint>
#include <span>

#include "core/audio/AudioStreamTypes.h"

namespace airplaywin::audio {

class IAudioDecoder {
public:
    virtual ~IAudioDecoder() = default;

    [[nodiscard]] virtual bool Configure(const EncodedAudioFormat& format) = 0;
    [[nodiscard]] virtual DecodeResult Decode(const EncodedAudioFrame& frame,
                                              std::span<float> output) noexcept = 0;
    [[nodiscard]] virtual DecodeResult ConcealLoss(std::uint32_t frame_count,
                                                   std::span<float> output) noexcept = 0;
    virtual void Reset() noexcept = 0;
};

}  // namespace airplaywin::audio
