#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "core/audio/AudioTypes.h"

namespace airplaywin::audio {

enum class AudioCodec : std::uint8_t {
    PcmL16BigEndian,
    AppleLossless,
};

struct EncodedAudioFormat final {
    AudioCodec codec{AudioCodec::PcmL16BigEndian};
    std::uint32_t sample_rate{44'100U};
    std::uint16_t channel_count{2U};
    std::uint8_t payload_type{96U};
    std::uint32_t nominal_frames_per_packet{352U};
    std::array<std::byte, 24U> codec_config{};
    std::uint8_t codec_config_size{0U};
    bool encrypted{false};
    std::array<std::byte, 16U> encryption_key{};
    std::array<std::byte, 16U> encryption_iv{};

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return sample_rate >= 8'000U && sample_rate <= 384'000U && channel_count >= 1U &&
               channel_count <= 8U && payload_type <= 127U &&
               nominal_frames_per_packet >= 1U && nominal_frames_per_packet <= 8'192U &&
               (codec != AudioCodec::AppleLossless || codec_config_size == codec_config.size());
    }

    [[nodiscard]] constexpr AudioFormat DecodedFormat() const noexcept {
        return AudioFormat{.sample_rate = sample_rate, .channel_count = channel_count};
    }
};

struct EncodedAudioFrame final {
    std::span<const std::byte> payload{};
    std::uint64_t extended_sequence_number{0U};
    std::uint32_t rtp_timestamp{0U};
    bool marker{false};
};

enum class DecodeStatus : std::uint8_t {
    Ok,
    NotConfigured,
    MalformedPayload,
    OutputTooSmall,
};

enum class DecodeFailurePoint : std::uint8_t {
    None,
    Decryption,
    InputBuffer,
    DecoderInput,
    DecoderOutput,
    OutputBuffer,
    OutputValidation,
};

struct DecodeResult final {
    DecodeStatus status{DecodeStatus::NotConfigured};
    std::uint32_t frame_count{0U};
    DecodeFailurePoint failure_point{DecodeFailurePoint::None};
    std::uint32_t platform_error{0U};
};

struct DecodedAudioFrameView final {
    std::span<const float> interleaved_samples{};
    std::uint32_t frame_count{0U};
    std::uint32_t rtp_timestamp{0U};
    std::uint64_t extended_sequence_number{0U};
    std::optional<std::int64_t> target_qpc{};
    bool concealed{false};
};

struct AudioSinkFeedback final {
    std::uint32_t queued_frames{0U};
    std::uint64_t underrun_count{0U};
    std::uint64_t output_latency_microseconds{0U};
};

}  // namespace airplaywin::audio
