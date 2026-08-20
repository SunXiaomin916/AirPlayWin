#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace airplaywin::audio {

enum class SampleFormat : std::uint8_t {
    Float32,
};

struct AudioFormat final {
    std::uint32_t sample_rate{48'000};
    std::uint16_t channel_count{2};
    SampleFormat sample_format{SampleFormat::Float32};

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return sample_rate >= 8'000U && sample_rate <= 384'000U &&
               channel_count >= 1U && channel_count <= 8U &&
               sample_format == SampleFormat::Float32;
    }
};

struct AudioBuffer final {
    std::span<const float> interleaved_samples{};
    std::uint64_t epoch_id{0};
    std::int64_t timestamp_qpc{0};
    std::uint32_t frame_count{0};
    std::uint32_t sample_rate{0};
    std::uint16_t channel_count{0};

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        const auto expected_samples =
            static_cast<std::size_t>(frame_count) * static_cast<std::size_t>(channel_count);
        return epoch_id != 0U && frame_count != 0U && sample_rate != 0U &&
               channel_count != 0U && interleaved_samples.size() == expected_samples;
    }
};

enum class AudioEpochReason : std::uint8_t {
    Initial,
    Flush,
    Seek,
    FormatChange,
    SenderReplace,
    HardResync,
    SessionReset,
};

enum class AudioTransition : std::uint8_t {
    None,
    Start,
    Resume,
    Stop,
    Pause,
    Flush,
    Seek,
    HardResync,
    DeviceSwitch,
};

enum class AudioTransitionState : std::uint8_t {
    Stopped,
    Prewarming,
    FadingIn,
    Audible,
    FadingOut,
    Paused,
    SafeMute,
    DeviceMuted,
};

struct AudioDiagnosticsSnapshot final {
    std::uint32_t current_buffer_depth_frames{0};
    std::uint64_t underrun_count{0};
    std::uint64_t rendered_frames{0};
    std::uint64_t dropped_stale_epoch_buffers{0};
    std::uint64_t invalid_numeric_samples{0};
    std::uint64_t clipped_samples{0};
    std::uint64_t dc_offset_events{0};
    std::uint64_t transition_requests{0};
    std::uint64_t fade_in_events{0};
    std::uint64_t fade_out_events{0};
    std::uint64_t safe_mute_events{0};
    std::uint64_t hard_resync_events{0};
    std::uint64_t underrun_transition_events{0};
    std::uint64_t click_pop_analyzed_frames{0};
    std::uint64_t click_pop_events{0};
    std::uint64_t click_pop_last_event_frame{0};
    float click_pop_maximum_step{0.0F};
    float click_pop_recent_peak{0.0F};
    std::uint64_t output_latency_microseconds{0};
    std::uint64_t device_switch_events{0};
    std::uint64_t device_recovery_attempts{0};
    std::uint64_t device_recovery_successes{0};
    std::uint64_t device_recovery_failures{0};
    std::wstring current_device_id{};
    std::uint32_t current_sample_rate{0};
    std::uint64_t current_audio_epoch{0};
    std::uint32_t last_output_error{0};
    bool output_recovering{false};
    AudioTransitionState transition_state{AudioTransitionState::Stopped};
};

}  // namespace airplaywin::audio
