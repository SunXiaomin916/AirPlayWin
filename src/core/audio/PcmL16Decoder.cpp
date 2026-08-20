#include "core/audio/PcmL16Decoder.h"

#include <algorithm>
#include <limits>

namespace airplaywin::audio {

bool PcmL16Decoder::Configure(const EncodedAudioFormat& format) {
    if (!format.IsValid() || format.codec != AudioCodec::PcmL16BigEndian) {
        return false;
    }
    format_ = format;
    configured_ = true;
    return true;
}

DecodeResult PcmL16Decoder::Decode(const EncodedAudioFrame& frame,
                                   const std::span<float> output) noexcept {
    if (!configured_) {
        return {.status = DecodeStatus::NotConfigured};
    }
    const auto bytes_per_frame = static_cast<std::size_t>(format_.channel_count) * 2U;
    if (frame.payload.empty() || frame.payload.size() % bytes_per_frame != 0U) {
        return {.status = DecodeStatus::MalformedPayload};
    }
    const auto frame_count = frame.payload.size() / bytes_per_frame;
    const auto sample_count = frame_count * format_.channel_count;
    if (frame_count > std::numeric_limits<std::uint32_t>::max() || output.size() < sample_count) {
        return {.status = DecodeStatus::OutputTooSmall};
    }
    constexpr float kScale = 1.0F / 32'768.0F;
    for (std::size_t sample = 0U; sample < sample_count; ++sample) {
        const auto offset = sample * 2U;
        const auto high = std::to_integer<std::uint8_t>(frame.payload[offset]);
        const auto low = std::to_integer<std::uint8_t>(frame.payload[offset + 1U]);
        const auto bits = static_cast<std::uint16_t>((static_cast<std::uint16_t>(high) << 8U) |
                                                     low);
        output[sample] = static_cast<float>(static_cast<std::int16_t>(bits)) * kScale;
    }
    return {.status = DecodeStatus::Ok,
            .frame_count = static_cast<std::uint32_t>(frame_count)};
}

DecodeResult PcmL16Decoder::ConcealLoss(const std::uint32_t frame_count,
                                        const std::span<float> output) noexcept {
    if (!configured_) {
        return {.status = DecodeStatus::NotConfigured};
    }
    const auto sample_count = static_cast<std::size_t>(frame_count) * format_.channel_count;
    if (frame_count == 0U || output.size() < sample_count) {
        return {.status = DecodeStatus::OutputTooSmall};
    }
    std::ranges::fill(output.first(sample_count), 0.0F);
    return {.status = DecodeStatus::Ok, .frame_count = frame_count};
}

void PcmL16Decoder::Reset() noexcept {}

}  // namespace airplaywin::audio
