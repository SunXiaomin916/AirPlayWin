#include "core/audio/AudioTransitionGuard.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace airplaywin::audio {

AudioTransitionGuard::AudioTransitionGuard(const AudioTransitionConfig& config)
    : config_(config),
      fade_in_frames_(MillisecondsToFrames(config.fade_in_milliseconds, config.sample_rate)),
      fade_out_frames_(MillisecondsToFrames(config.fade_out_milliseconds, config.sample_rate)),
      volume_ramp_frames_(
          MillisecondsToFrames(config.volume_ramp_milliseconds, config.sample_rate)),
      prewarm_frames_(MillisecondsToFrames(config.prewarm_milliseconds, config.sample_rate)) {
    if (config.sample_rate == 0U || config.maximum_pcm_amplitude <= 0.0F ||
        config.maximum_pcm_amplitude > 1.0F || config.dc_offset_threshold <= 0.0F) {
        throw std::invalid_argument("Invalid AudioTransitionConfig");
    }
}

void AudioTransitionGuard::Request(const AudioTransition transition) noexcept {
    if (transition != AudioTransition::None) {
        if ((transition == AudioTransition::Start || transition == AudioTransition::Resume) &&
            pending_command_.load(std::memory_order_acquire) == AudioTransition::None) {
            const auto state = State();
            if (state == AudioTransitionState::Prewarming ||
                state == AudioTransitionState::FadingIn ||
                state == AudioTransitionState::Audible) {
                return;
            }
        }
        pending_command_.store(transition, std::memory_order_release);
    }
}

void AudioTransitionGuard::SetTargetVolume(const float linear_gain) noexcept {
    const auto safe_gain = std::isfinite(linear_gain) ? std::clamp(linear_gain, 0.0F, 1.0F) : 0.0F;
    target_volume_.store(safe_gain, std::memory_order_release);
}

void AudioTransitionGuard::Process(const std::span<float> interleaved_samples,
                                   const std::uint32_t frame_count,
                                   const std::uint16_t channel_count,
                                   const bool underrun) noexcept {
    const auto channels = static_cast<std::size_t>(channel_count);
    const auto required_samples = static_cast<std::size_t>(frame_count) * channels;
    if (frame_count == 0U || channel_count == 0U || channels > kMaximumChannels ||
        interleaved_samples.size() < required_samples) {
        return;
    }

    ApplyPendingCommand();
    if (underrun && (render_state_ == AudioTransitionState::Audible ||
                     render_state_ == AudioTransitionState::FadingIn)) {
        BeginFadeOut(AudioTransition::None);
    }

    for (std::uint32_t frame = 0U; frame < frame_count; ++frame) {
        UpdateVolumeRamp();

        float transition_gain = 0.0F;
        if (render_state_ == AudioTransitionState::FadingIn) {
            const auto completed = fade_in_frames_ - transition_frames_remaining_ + 1U;
            transition_gain = SmoothStep(static_cast<float>(completed) /
                                         static_cast<float>(fade_in_frames_));
        } else if (render_state_ == AudioTransitionState::Audible) {
            transition_gain = 1.0F;
        } else if (render_state_ == AudioTransitionState::FadingOut) {
            transition_gain = SmoothStep(static_cast<float>(transition_frames_remaining_) /
                                         static_cast<float>(fade_out_frames_));
        }

        for (std::size_t channel = 0U; channel < channels; ++channel) {
            const auto sample_index = static_cast<std::size_t>(frame) * channels + channel;
            auto input = interleaved_samples[sample_index];
            if (!std::isfinite(input)) {
                input = 0.0F;
                invalid_numeric_samples_.fetch_add(1U, std::memory_order_relaxed);
            }
            if (std::abs(input) > config_.maximum_pcm_amplitude) {
                input = std::clamp(input, -config_.maximum_pcm_amplitude,
                                   config_.maximum_pcm_amplitude);
                clipped_samples_.fetch_add(1U, std::memory_order_relaxed);
            }
            if (std::fpclassify(input) == FP_SUBNORMAL) {
                input = 0.0F;
            }

            constexpr double kDcAlpha = 0.0001;
            dc_estimate_[channel] += kDcAlpha *
                                     (static_cast<double>(input) - dc_estimate_[channel]);
            const auto absolute_dc = std::abs(dc_estimate_[channel]);
            if (absolute_dc > static_cast<double>(config_.dc_offset_threshold) &&
                !dc_latched_[channel]) {
                dc_latched_[channel] = true;
                dc_offset_events_.fetch_add(1U, std::memory_order_relaxed);
            } else if (absolute_dc < static_cast<double>(config_.dc_offset_threshold) * 0.5) {
                dc_latched_[channel] = false;
            }

            float output = 0.0F;
            if (render_state_ == AudioTransitionState::FadingOut) {
                output = fade_tail_[channel] * transition_gain;
            } else if (render_state_ == AudioTransitionState::FadingIn ||
                       render_state_ == AudioTransitionState::Audible) {
                output = input * transition_gain * current_volume_;
            }
            output = std::clamp(output, -1.0F, 1.0F);
            interleaved_samples[sample_index] = output;
            last_output_[channel] = output;
        }

        if (render_state_ == AudioTransitionState::Prewarming) {
            if (transition_frames_remaining_ > 0U) {
                --transition_frames_remaining_;
            }
            if (transition_frames_remaining_ == 0U) {
                render_state_ = AudioTransitionState::FadingIn;
                transition_frames_remaining_ = fade_in_frames_;
            }
        } else if (render_state_ == AudioTransitionState::FadingIn) {
            if (transition_frames_remaining_ > 0U) {
                --transition_frames_remaining_;
            }
            if (transition_frames_remaining_ == 0U) {
                render_state_ = AudioTransitionState::Audible;
            }
        } else if (render_state_ == AudioTransitionState::FadingOut) {
            if (transition_frames_remaining_ > 0U) {
                --transition_frames_remaining_;
            }
            if (transition_frames_remaining_ == 0U) {
                CompleteFadeOut();
            }
        }
    }

    public_state_.store(render_state_, std::memory_order_release);
}

AudioTransitionState AudioTransitionGuard::State() const noexcept {
    return public_state_.load(std::memory_order_acquire);
}

bool AudioTransitionGuard::ShouldHoldInput() const noexcept {
    const auto pending = pending_command_.load(std::memory_order_acquire);
    if (pending == AudioTransition::Start || pending == AudioTransition::Resume) {
        return false;
    }
    const auto state = State();
    return state == AudioTransitionState::Stopped || state == AudioTransitionState::Paused ||
           state == AudioTransitionState::SafeMute ||
           state == AudioTransitionState::DeviceMuted;
}

bool AudioTransitionGuard::HasPendingCommand() const noexcept {
    return pending_command_.load(std::memory_order_acquire) != AudioTransition::None;
}

std::uint64_t AudioTransitionGuard::InvalidNumericSamples() const noexcept {
    return invalid_numeric_samples_.load(std::memory_order_relaxed);
}

std::uint64_t AudioTransitionGuard::ClippedSamples() const noexcept {
    return clipped_samples_.load(std::memory_order_relaxed);
}

std::uint64_t AudioTransitionGuard::DcOffsetEvents() const noexcept {
    return dc_offset_events_.load(std::memory_order_relaxed);
}

std::uint32_t AudioTransitionGuard::MillisecondsToFrames(const float milliseconds,
                                                         const std::uint32_t sample_rate) noexcept {
    if (!std::isfinite(milliseconds) || milliseconds <= 0.0F || sample_rate == 0U) {
        return 1U;
    }
    const auto frames = static_cast<double>(milliseconds) *
                        static_cast<double>(sample_rate) / 1'000.0;
    return std::max(1U, static_cast<std::uint32_t>(std::ceil(frames)));
}

float AudioTransitionGuard::SmoothStep(const float value) noexcept {
    const auto x = std::clamp(value, 0.0F, 1.0F);
    return x * x * (3.0F - 2.0F * x);
}

void AudioTransitionGuard::ApplyPendingCommand() noexcept {
    const auto command = pending_command_.exchange(AudioTransition::None, std::memory_order_acq_rel);
    switch (command) {
    case AudioTransition::Start:
    case AudioTransition::Resume:
        BeginPrewarm();
        break;
    case AudioTransition::Stop:
    case AudioTransition::Pause:
    case AudioTransition::Flush:
    case AudioTransition::Seek:
    case AudioTransition::HardResync:
    case AudioTransition::DeviceSwitch:
        BeginFadeOut(command);
        break;
    case AudioTransition::None:
        break;
    }
}

void AudioTransitionGuard::BeginFadeOut(const AudioTransition reason) noexcept {
    fade_out_reason_ = reason;
    fade_tail_ = last_output_;
    transition_frames_remaining_ = fade_out_frames_;
    render_state_ = AudioTransitionState::FadingOut;
}

void AudioTransitionGuard::CompleteFadeOut() noexcept {
    switch (fade_out_reason_) {
    case AudioTransition::Stop:
        render_state_ = AudioTransitionState::Stopped;
        break;
    case AudioTransition::Pause:
        render_state_ = AudioTransitionState::Paused;
        break;
    case AudioTransition::DeviceSwitch:
        render_state_ = AudioTransitionState::DeviceMuted;
        break;
    case AudioTransition::Flush:
    case AudioTransition::Seek:
    case AudioTransition::HardResync:
        render_state_ = AudioTransitionState::SafeMute;
        break;
    case AudioTransition::None:
        render_state_ = AudioTransitionState::SafeMute;
        break;
    case AudioTransition::Start:
    case AudioTransition::Resume:
        BeginPrewarm();
        break;
    }
    fade_out_reason_ = AudioTransition::None;
}

void AudioTransitionGuard::BeginPrewarm() noexcept {
    render_state_ = AudioTransitionState::Prewarming;
    transition_frames_remaining_ = prewarm_frames_;
    last_output_.fill(0.0F);
}

void AudioTransitionGuard::UpdateVolumeRamp() noexcept {
    const auto target = target_volume_.load(std::memory_order_acquire);
    if (target != cached_target_volume_) {
        cached_target_volume_ = target;
        volume_frames_remaining_ = volume_ramp_frames_;
        volume_step_ = (cached_target_volume_ - current_volume_) /
                       static_cast<float>(volume_ramp_frames_);
    }
    if (volume_frames_remaining_ > 0U) {
        current_volume_ += volume_step_;
        --volume_frames_remaining_;
        if (volume_frames_remaining_ == 0U) {
            current_volume_ = cached_target_volume_;
        }
    }
}

}  // namespace airplaywin::audio
