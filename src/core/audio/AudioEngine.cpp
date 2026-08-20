#include "core/audio/AudioEngine.h"

#include <stdexcept>
#include <utility>

namespace airplaywin::audio {

AudioEngine::AudioEngine(std::unique_ptr<IAudioOutput> output) : output_(std::move(output)) {
    if (!output_) {
        throw std::invalid_argument("AudioEngine requires an audio output");
    }
}

bool AudioEngine::Open(const AudioFormat& format) {
    if (!format.IsValid() || is_open_.load(std::memory_order_acquire)) {
        return false;
    }
    if (!output_->Open(format)) {
        return false;
    }
    sample_rate_.store(format.sample_rate, std::memory_order_release);
    channel_count_.store(format.channel_count, std::memory_order_release);
    output_->BeginEpoch(epoch_.Current(), AudioTransition::None);
    is_open_.store(true, std::memory_order_release);
    return true;
}

bool AudioEngine::Start() {
    if (!is_open_.load(std::memory_order_acquire)) {
        return false;
    }
    const auto started = output_->Start();
    is_started_.store(started, std::memory_order_release);
    return started;
}

bool AudioEngine::Submit(const std::span<const float> interleaved_samples,
                         const std::uint32_t frame_count,
                         const std::int64_t timestamp_qpc) noexcept {
    if (!is_open_.load(std::memory_order_acquire)) {
        return false;
    }
    const auto channels = channel_count_.load(std::memory_order_acquire);
    const AudioBuffer buffer{
        .interleaved_samples = interleaved_samples,
        .epoch_id = epoch_.Current(),
        .timestamp_qpc = timestamp_qpc,
        .frame_count = frame_count,
        .sample_rate = sample_rate_.load(std::memory_order_acquire),
        .channel_count = channels,
    };
    return Submit(buffer);
}

bool AudioEngine::Submit(const AudioBuffer& buffer) noexcept {
    if (!is_open_.load(std::memory_order_acquire) || !buffer.IsValid() ||
        buffer.sample_rate != sample_rate_.load(std::memory_order_acquire) ||
        buffer.channel_count != channel_count_.load(std::memory_order_acquire)) {
        return false;
    }
    return output_->Write(buffer);
}

void AudioEngine::Pause() noexcept {
    output_->Pause();
}

void AudioEngine::Resume() noexcept {
    output_->Resume();
}

void AudioEngine::Stop() noexcept {
    output_->Stop();
    is_started_.store(false, std::memory_order_release);
}

void AudioEngine::Flush() noexcept {
    AdvanceEpoch(AudioEpochReason::Flush, AudioTransition::Flush);
}

void AudioEngine::Seek() noexcept {
    AdvanceEpoch(AudioEpochReason::Seek, AudioTransition::Seek);
}

void AudioEngine::HardResync() noexcept {
    AdvanceEpoch(AudioEpochReason::HardResync, AudioTransition::HardResync);
}

void AudioEngine::ReplaceSender() noexcept {
    AdvanceEpoch(AudioEpochReason::SenderReplace, AudioTransition::Flush);
}

void AudioEngine::ResetSession() noexcept {
    AdvanceEpoch(AudioEpochReason::SessionReset, AudioTransition::Flush);
}

bool AudioEngine::ChangeFormat(const AudioFormat& format) {
    if (!format.IsValid() || !is_open_.load(std::memory_order_acquire)) {
        return false;
    }

    const auto restart = is_started_.load(std::memory_order_acquire);
    output_->Stop();
    output_->Close();
    is_open_.store(false, std::memory_order_release);

    const auto next_epoch = epoch_.Advance(AudioEpochReason::FormatChange);
    if (!output_->Open(format)) {
        is_started_.store(false, std::memory_order_release);
        return false;
    }
    sample_rate_.store(format.sample_rate, std::memory_order_release);
    channel_count_.store(format.channel_count, std::memory_order_release);
    output_->BeginEpoch(next_epoch, AudioTransition::Start);
    is_open_.store(true, std::memory_order_release);
    if (restart && !output_->Start()) {
        is_started_.store(false, std::memory_order_release);
        return false;
    }
    is_started_.store(restart, std::memory_order_release);
    return true;
}

void AudioEngine::SetVolume(const float linear_gain) noexcept {
    output_->SetVolume(linear_gain);
}

void AudioEngine::Close() noexcept {
    if (is_open_.exchange(false, std::memory_order_acq_rel)) {
        output_->Close();
    }
    is_started_.store(false, std::memory_order_release);
}

std::uint64_t AudioEngine::CurrentEpoch() const noexcept {
    return epoch_.Current();
}

AudioDiagnosticsSnapshot AudioEngine::Diagnostics() const {
    return output_->Diagnostics();
}

void AudioEngine::AdvanceEpoch(const AudioEpochReason reason,
                               const AudioTransition transition) noexcept {
    const auto next_epoch = epoch_.Advance(reason);
    output_->BeginEpoch(next_epoch, transition);
}

}  // namespace airplaywin::audio
