#include "platform/windows/audio/WindowsAudioStreamSink.h"

#include <algorithm>
#include <utility>

#include "platform/windows/timing/QpcClock.h"

namespace airplaywin::windows::audio {

WindowsAudioStreamSink::WindowsAudioStreamSink(WasapiOutputOptions options,
                                               const std::uint32_t prefill_milliseconds)
    : engine_(std::move(options)),
      prefill_milliseconds_(std::clamp(prefill_milliseconds, 5U, 500U)) {}

bool WindowsAudioStreamSink::Configure(const airplaywin::audio::AudioFormat& format) {
    if (!format.IsValid()) {
        return false;
    }
    std::scoped_lock lock{mutex_};
    if (configured_) {
        engine_.Stop();
        engine_.Close();
    }
    if (!engine_.Open(format)) {
        configured_ = false;
        return false;
    }
    format_ = format;
    prefilled_frames_ = 0U;
    configured_ = true;
    armed_ = false;
    started_ = false;
    resume_after_prefill_ = false;
    return true;
}

bool WindowsAudioStreamSink::Start() {
    std::scoped_lock lock{mutex_};
    if (!configured_) {
        return false;
    }
    armed_ = true;
    return true;
}

bool WindowsAudioStreamSink::Submit(
    const airplaywin::audio::DecodedAudioFrameView& frame) noexcept {
    std::scoped_lock lock{mutex_};
    if (!configured_ || !armed_ || frame.frame_count == 0U ||
        frame.interleaved_samples.size() !=
            static_cast<std::size_t>(frame.frame_count) * format_.channel_count) {
        rejected_frames_ += frame.frame_count;
        return false;
    }
    if (!engine_.Submit(frame.interleaved_samples, frame.frame_count,
                        windows::timing::QpcClock::Now())) {
        rejected_frames_ += frame.frame_count;
        return false;
    }
    accepted_frames_ += frame.frame_count;
    prefilled_frames_ += frame.frame_count;
    const auto required_prefill =
        static_cast<std::uint64_t>(format_.sample_rate) * prefill_milliseconds_ / 1'000U;
    if (prefilled_frames_ >= required_prefill) {
        if (!started_) {
            started_ = engine_.Start();
            if (!started_) {
                return false;
            }
        } else if (resume_after_prefill_) {
            engine_.Resume();
            resume_after_prefill_ = false;
        }
    }
    return true;
}

void WindowsAudioStreamSink::Pause() noexcept {
    std::scoped_lock lock{mutex_};
    armed_ = false;
    if (started_) {
        engine_.Pause();
    }
}

void WindowsAudioStreamSink::Resume() noexcept {
    std::scoped_lock lock{mutex_};
    if (!configured_) {
        return;
    }
    armed_ = true;
    if (started_ && !resume_after_prefill_) {
        engine_.Resume();
    }
}

void WindowsAudioStreamSink::Flush() noexcept {
    std::scoped_lock lock{mutex_};
    if (!configured_) {
        return;
    }
    if (started_) {
        engine_.Pause();
        resume_after_prefill_ = true;
    }
    engine_.Flush();
    prefilled_frames_ = 0U;
}

void WindowsAudioStreamSink::SetVolume(const float linear_gain) noexcept {
    std::scoped_lock lock{mutex_};
    if (configured_) {
        engine_.SetVolume(linear_gain);
    }
}

void WindowsAudioStreamSink::Stop() noexcept {
    std::scoped_lock lock{mutex_};
    if (!configured_) {
        return;
    }
    engine_.Stop();
    engine_.ResetSession();
    engine_.Close();
    prefilled_frames_ = 0U;
    configured_ = false;
    armed_ = false;
    started_ = false;
    resume_after_prefill_ = false;
}

AudioStreamSinkDiagnostics WindowsAudioStreamSink::Diagnostics() const {
    std::scoped_lock lock{mutex_};
    return AudioStreamSinkDiagnostics{
        .configured = configured_,
        .armed = armed_,
        .started = started_,
        .accepted_frames = accepted_frames_,
        .rejected_frames = rejected_frames_,
        .output = engine_.Diagnostics(),
    };
}

}  // namespace airplaywin::windows::audio
