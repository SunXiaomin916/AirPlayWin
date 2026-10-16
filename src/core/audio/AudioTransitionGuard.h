#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>

#include "core/audio/AudioTypes.h"
#include "core/audio/ClickPopDetector.h"

namespace airplaywin::audio {

struct AudioTransitionConfig final {
    std::uint32_t sample_rate{48'000};
    float fade_in_milliseconds{10.0F};
    float fade_out_milliseconds{10.0F};
    float volume_ramp_milliseconds{20.0F};
    float prewarm_milliseconds{10.0F};
    float maximum_pcm_amplitude{1.0F};
    float dc_offset_threshold{0.15F};
    float click_pop_step_threshold{0.35F};
};

class AudioTransitionGuard final {
public:
    static constexpr std::size_t kMaximumChannels = 8U;

    explicit AudioTransitionGuard(const AudioTransitionConfig& config);

    void Request(AudioTransition transition) noexcept;
    void SetTargetVolume(float linear_gain) noexcept;
    // Processes one render callback. `valid_frames` is the number of leading
    // frames that contain real queued PCM; any remaining frames are
    // underrun fill (zeroed) and may only carry fade-tail material.
    void Process(std::span<float> interleaved_samples,
                 std::uint32_t frame_count,
                 std::uint16_t channel_count,
                 std::uint32_t valid_frames) noexcept;

    [[nodiscard]] AudioTransitionState State() const noexcept;
    [[nodiscard]] bool ShouldHoldInput() const noexcept;
    [[nodiscard]] bool HasPendingCommand() const noexcept;
    [[nodiscard]] std::uint64_t InvalidNumericSamples() const noexcept;
    [[nodiscard]] std::uint64_t ClippedSamples() const noexcept;
    [[nodiscard]] std::uint64_t DcOffsetEvents() const noexcept;
    [[nodiscard]] std::uint64_t TransitionRequests() const noexcept;
    [[nodiscard]] std::uint64_t FadeInEvents() const noexcept;
    [[nodiscard]] std::uint64_t FadeOutEvents() const noexcept;
    [[nodiscard]] std::uint64_t SafeMuteEvents() const noexcept;
    [[nodiscard]] std::uint64_t HardResyncEvents() const noexcept;
    [[nodiscard]] std::uint64_t UnderrunTransitionEvents() const noexcept;
    [[nodiscard]] ClickPopDiagnostics ClickPop() const noexcept;

private:
    [[nodiscard]] static std::uint32_t MillisecondsToFrames(float milliseconds,
                                                            std::uint32_t sample_rate) noexcept;
    [[nodiscard]] static float SmoothStep(float value) noexcept;
    [[nodiscard]] static std::uint8_t CommandPriority(AudioTransition transition) noexcept;
    void ApplyPendingCommand() noexcept;
    void PushDeferred(AudioTransition transition) noexcept;
    [[nodiscard]] AudioTransition PopDeferred() noexcept;
    [[nodiscard]] bool DeferredQueueEmpty() const noexcept;
    void BeginFadeOut(AudioTransition reason) noexcept;
    void CompleteFadeOut() noexcept;
    void BeginPrewarm() noexcept;
    void UpdateVolumeRamp() noexcept;

    static constexpr std::size_t kDeferredQueueCapacity = 8U;

    const AudioTransitionConfig config_;
    const std::uint32_t fade_in_frames_;
    const std::uint32_t fade_out_frames_;
    const std::uint32_t volume_ramp_frames_;
    const std::uint32_t prewarm_frames_;
    ClickPopDetector click_pop_detector_;

    std::atomic<AudioTransition> pending_command_{AudioTransition::None};
    std::atomic<AudioTransitionState> public_state_{AudioTransitionState::Stopped};
    std::atomic<float> target_volume_{1.0F};
    std::atomic<std::uint64_t> invalid_numeric_samples_{0U};
    std::atomic<std::uint64_t> clipped_samples_{0U};
    std::atomic<std::uint64_t> dc_offset_events_{0U};
    std::atomic<std::uint64_t> transition_requests_{0U};
    std::atomic<std::uint64_t> fade_in_events_{0U};
    std::atomic<std::uint64_t> fade_out_events_{0U};
    std::atomic<std::uint64_t> safe_mute_events_{0U};
    std::atomic<std::uint64_t> hard_resync_events_{0U};
    std::atomic<std::uint64_t> underrun_transition_events_{0U};

    AudioTransitionState render_state_{AudioTransitionState::Stopped};
    AudioTransition fade_out_reason_{AudioTransition::None};
    bool restart_after_fade_out_{false};
    std::uint32_t transition_frames_remaining_{0U};
    float current_volume_{1.0F};
    float cached_target_volume_{1.0F};
    float volume_step_{0.0F};
    std::uint32_t volume_frames_remaining_{0U};
    std::array<float, kMaximumChannels> last_output_{};
    std::array<float, kMaximumChannels> fade_tail_{};
    std::array<double, kMaximumChannels> dc_estimate_{};
    std::array<bool, kMaximumChannels> dc_latched_{};
    // Lower-priority commands displaced by the priority scheme are deferred
    // here instead of being discarded, so a Resume racing a pending Pause
    // cannot strand playback in silence. Producers push under a short
    // mutex; the render thread pops when a pending command is consumed.
    mutable std::mutex deferred_mutex_{};
    std::array<AudioTransition, kDeferredQueueCapacity> deferred_queue_{};
    std::size_t deferred_head_{0U};
    std::size_t deferred_count_{0U};
};

}  // namespace airplaywin::audio
