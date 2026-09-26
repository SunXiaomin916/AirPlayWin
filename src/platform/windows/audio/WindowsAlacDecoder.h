#pragma once

#include <memory>

#include "core/audio/IAudioDecoder.h"

namespace airplaywin::windows::audio {

class WindowsAlacDecoder final : public airplaywin::audio::IAudioDecoder {
public:
    WindowsAlacDecoder();
    ~WindowsAlacDecoder() override;

    WindowsAlacDecoder(const WindowsAlacDecoder&) = delete;
    WindowsAlacDecoder& operator=(const WindowsAlacDecoder&) = delete;

    [[nodiscard]] bool Configure(
        const airplaywin::audio::EncodedAudioFormat& format) override;
    [[nodiscard]] airplaywin::audio::DecodeResult Decode(
        const airplaywin::audio::EncodedAudioFrame& frame,
        std::span<float> output) noexcept override;
    [[nodiscard]] airplaywin::audio::DecodeResult ConcealLoss(
        std::uint32_t frame_count,
        std::span<float> output) noexcept override;
    void Reset() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace airplaywin::windows::audio
