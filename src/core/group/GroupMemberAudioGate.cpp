#include "core/group/GroupMemberAudioGate.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace airplaywin::group {

GroupMemberAudioGate::GroupMemberAudioGate(
    audio::IAudioFrameSink& sink,
    std::shared_ptr<GroupMemberRuntime> runtime)
    : sink_(sink), runtime_(std::move(runtime)) {
    if (!runtime_) {
        throw std::invalid_argument("group member runtime is required");
    }
}

bool GroupMemberAudioGate::Configure(const audio::AudioFormat& format) {
    if (!format.IsValid()) {
        return false;
    }
    configured_.store(false, std::memory_order_release);
    try {
        silence_.assign(static_cast<std::size_t>(kMaximumFrameCount) *
                            format.channel_count,
                        0.0F);
    } catch (...) {
        silence_.clear();
        return false;
    }
    if (!sink_.Configure(format)) {
        silence_.clear();
        return false;
    }
    channel_count_.store(format.channel_count, std::memory_order_release);
    audible_.store(false, std::memory_order_release);
    sink_.SetVolume(0.0F);
    configured_.store(true, std::memory_order_release);
    return true;
}

bool GroupMemberAudioGate::Start() {
    if (!configured_.load(std::memory_order_acquire)) {
        return false;
    }
    audible_.store(false, std::memory_order_release);
    sink_.SetVolume(0.0F);
    return sink_.Start();
}

bool GroupMemberAudioGate::Submit(
    const audio::DecodedAudioFrameView& frame) noexcept {
    const auto channels = channel_count_.load(std::memory_order_acquire);
    const auto sample_count = static_cast<std::size_t>(frame.frame_count) * channels;
    if (!configured_.load(std::memory_order_acquire) || frame.frame_count == 0U ||
        frame.frame_count > kMaximumFrameCount || channels == 0U ||
        frame.interleaved_samples.size() != sample_count ||
        sample_count > silence_.size()) {
        rejected_frames_.fetch_add(frame.frame_count, std::memory_order_relaxed);
        return false;
    }

    const bool render_audibly = ShouldRenderAudibly(frame);
    ApplyAudibleState(render_audibly);
    if (render_audibly) {
        if (!sink_.Submit(frame)) {
            rejected_frames_.fetch_add(frame.frame_count, std::memory_order_relaxed);
            return false;
        }
        audible_frames_.fetch_add(frame.frame_count, std::memory_order_relaxed);
        return true;
    }

    const audio::DecodedAudioFrameView muted{
        .interleaved_samples = std::span<const float>{silence_.data(), sample_count},
        .frame_count = frame.frame_count,
        .rtp_timestamp = frame.rtp_timestamp,
        .extended_sequence_number = frame.extended_sequence_number,
        .target_qpc = frame.target_qpc,
        .concealed = true,
    };
    if (!sink_.Submit(muted)) {
        rejected_frames_.fetch_add(frame.frame_count, std::memory_order_relaxed);
        return false;
    }
    muted_frames_.fetch_add(frame.frame_count, std::memory_order_relaxed);
    return true;
}

void GroupMemberAudioGate::Pause() noexcept {
    ApplyAudibleState(false);
    sink_.Pause();
}

void GroupMemberAudioGate::Resume() noexcept {
    sink_.Resume();
}

void GroupMemberAudioGate::Flush() noexcept {
    ApplyAudibleState(false);
    sink_.Flush();
}

void GroupMemberAudioGate::HardResync() noexcept {
    ApplyAudibleState(false);
    sink_.HardResync();
}

void GroupMemberAudioGate::SetVolume(const float linear_gain) noexcept {
    const auto safe_gain = std::isfinite(linear_gain) ? std::clamp(linear_gain, 0.0F, 1.0F)
                                                      : 0.0F;
    desired_gain_.store(safe_gain, std::memory_order_release);
    if (audible_.load(std::memory_order_acquire)) {
        sink_.SetVolume(safe_gain);
    }
}

void GroupMemberAudioGate::Stop() noexcept {
    ApplyAudibleState(false);
    configured_.store(false, std::memory_order_release);
    channel_count_.store(0U, std::memory_order_release);
    sink_.Stop();
}

audio::AudioSinkFeedback GroupMemberAudioGate::Feedback() const noexcept {
    return sink_.Feedback();
}

GroupMemberAudioGateDiagnostics GroupMemberAudioGate::Diagnostics() const noexcept {
    return {
        .configured = configured_.load(std::memory_order_acquire),
        .audible = audible_.load(std::memory_order_acquire),
        .muted_frames = muted_frames_.load(std::memory_order_relaxed),
        .audible_frames = audible_frames_.load(std::memory_order_relaxed),
        .rejected_frames = rejected_frames_.load(std::memory_order_relaxed),
        .ramp_in_events = ramp_in_events_.load(std::memory_order_relaxed),
        .ramp_out_events = ramp_out_events_.load(std::memory_order_relaxed),
    };
}

bool GroupMemberAudioGate::ShouldRenderAudibly(
    const audio::DecodedAudioFrameView& frame) const noexcept {
    const auto state = runtime_->State();
    if (state == GroupMemberState::Active || state == GroupMemberState::Holdover) {
        return true;
    }
    if (state != GroupMemberState::MutedReady ||
        runtime_->ActivationRemotePtpNanoseconds() == 0U) {
        return false;
    }
    const auto activation = runtime_->ActivationRtpTimestamp();
    return static_cast<std::int32_t>(frame.rtp_timestamp - activation) >= 0;
}

void GroupMemberAudioGate::ApplyAudibleState(const bool audible) noexcept {
    const bool previous = audible_.exchange(audible, std::memory_order_acq_rel);
    if (previous == audible) {
        return;
    }
    if (audible) {
        sink_.SetVolume(desired_gain_.load(std::memory_order_acquire));
        ramp_in_events_.fetch_add(1U, std::memory_order_relaxed);
    } else {
        sink_.SetVolume(0.0F);
        ramp_out_events_.fetch_add(1U, std::memory_order_relaxed);
    }
}

}  // namespace airplaywin::group
