#pragma once

#include "core/audio/IAudioDecoder.h"

namespace airplaywin::audio {

class PcmL16Decoder final : public IAudioDecoder {
public:
    [[nodiscard]] bool Configure(const EncodedAudioFormat& format) override;
    [[nodiscard]] DecodeResult Decode(const EncodedAudioFrame& frame,
                                      std::span<float> output) noexcept override;
    [[nodiscard]] DecodeResult ConcealLoss(std::uint32_t frame_count,
                                           std::span<float> output) noexcept override;
    void Reset() noexcept override;

private:
    EncodedAudioFormat format_{};
    bool configured_{false};
};

}  // namespace airplaywin::audio
