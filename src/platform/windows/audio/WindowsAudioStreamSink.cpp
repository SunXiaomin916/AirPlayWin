#include "platform/windows/audio/WindowsAudioStreamSink.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>
#include <utility>

#include "platform/windows/timing/QpcClock.h"

namespace airplaywin::windows::audio {

namespace {

[[nodiscard]] std::uint64_t TicksToMicroseconds(const std::int64_t ticks,
                                                const std::int64_t frequency) noexcept {
    if (ticks <= 0 || frequency <= 0) {
        return 0U;
    }
    const auto value = static_cast<std::uint64_t>(ticks);
    const auto rate = static_cast<std::uint64_t>(frequency);
    const auto whole_seconds = value / rate;
    if (whole_seconds > std::numeric_limits<std::uint64_t>::max() / 1'000'000U) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return whole_seconds * 1'000'000U + (value % rate) * 1'000'000U / rate;
}

}  // namespace

WindowsAudioStreamSink::WindowsAudioStreamSink(WasapiOutputOptions options,
                                               const std::uint32_t prefill_milliseconds,
                                               const std::uint32_t submission_lead_milliseconds)
    : engine_(std::move(options)),
      prefill_milliseconds_(std::clamp(prefill_milliseconds, 5U, 500U)),
      submission_lead_milliseconds_(
          std::clamp(submission_lead_milliseconds, 5U, 200U)) {}

WindowsAudioStreamSink::WindowsAudioStreamSink(
    std::unique_ptr<airplaywin::audio::IAudioOutput> output,
    const std::uint32_t prefill_milliseconds,
    const std::uint32_t submission_lead_milliseconds)
    : engine_(std::move(output)),
      prefill_milliseconds_(std::clamp(prefill_milliseconds, 5U, 500U)),
      submission_lead_milliseconds_(
          std::clamp(submission_lead_milliseconds, 5U, 200U)) {}

bool WindowsAudioStreamSink::Configure(const airplaywin::audio::AudioFormat& format) {
    if (!format.IsValid()) {
        return false;
    }
    std::scoped_lock lock{mutex_};
    schedule_generation_.fetch_add(1U, std::memory_order_acq_rel);
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
    std::uint64_t generation = 0U;
    {
        std::scoped_lock lock{mutex_};
        if (!configured_ || !armed_ || frame.frame_count == 0U ||
            frame.interleaved_samples.size() !=
                static_cast<std::size_t>(frame.frame_count) * format_.channel_count) {
            rejected_frames_ += frame.frame_count;
            return false;
        }
        generation = schedule_generation_.load(std::memory_order_acquire);
    }

    ScheduleWaitResult schedule;
    if (frame.target_qpc.has_value()) {
        schedule = WaitForSubmissionWindow(*frame.target_qpc, generation);
        if (!schedule.valid) {
            std::scoped_lock lock{mutex_};
            rejected_frames_ += frame.frame_count;
            return false;
        }
    }

    std::scoped_lock lock{mutex_};
    if (!configured_ || !armed_ ||
        schedule_generation_.load(std::memory_order_acquire) != generation) {
        rejected_frames_ += frame.frame_count;
        return false;
    }
    if (!engine_.Submit(frame.interleaved_samples, frame.frame_count,
                        frame.target_qpc.value_or(windows::timing::QpcClock::Now()))) {
        rejected_frames_ += frame.frame_count;
        return false;
    }
    accepted_frames_ += frame.frame_count;
    if (frame.target_qpc.has_value()) {
        scheduled_frames_ += frame.frame_count;
        scheduling_wait_microseconds_ += schedule.waited_microseconds;
        last_target_qpc_ = *frame.target_qpc;
        if (schedule.lateness_microseconds != 0U) {
            late_scheduled_frames_ += frame.frame_count;
            maximum_schedule_lateness_microseconds_ =
                std::max(maximum_schedule_lateness_microseconds_,
                         schedule.lateness_microseconds);
        }
    }
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
    schedule_generation_.fetch_add(1U, std::memory_order_acq_rel);
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
    schedule_generation_.fetch_add(1U, std::memory_order_acq_rel);
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
    schedule_generation_.fetch_add(1U, std::memory_order_acq_rel);
    if (started_) {
        engine_.Pause();
        resume_after_prefill_ = true;
    }
    engine_.Flush();
    prefilled_frames_ = 0U;
}

void WindowsAudioStreamSink::HardResync() noexcept {
    std::scoped_lock lock{mutex_};
    if (!configured_) {
        return;
    }
    schedule_generation_.fetch_add(1U, std::memory_order_acq_rel);
    if (started_) {
        engine_.Pause();
        resume_after_prefill_ = true;
    }
    engine_.HardResync();
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
    schedule_generation_.fetch_add(1U, std::memory_order_acq_rel);
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

airplaywin::audio::AudioSinkFeedback WindowsAudioStreamSink::Feedback() const noexcept {
    try {
        std::scoped_lock lock{mutex_};
        const auto output = engine_.Diagnostics();
        return {
            .queued_frames = output.current_buffer_depth_frames,
            .underrun_count = output.underrun_count,
            .output_latency_microseconds = output.output_latency_microseconds,
        };
    } catch (...) {
        return {};
    }
}

AudioStreamSinkDiagnostics WindowsAudioStreamSink::Diagnostics() const {
    std::scoped_lock lock{mutex_};
    return AudioStreamSinkDiagnostics{
        .configured = configured_,
        .armed = armed_,
        .started = started_,
        .accepted_frames = accepted_frames_,
        .rejected_frames = rejected_frames_,
        .scheduled_frames = scheduled_frames_,
        .late_scheduled_frames = late_scheduled_frames_,
        .scheduling_wait_microseconds = scheduling_wait_microseconds_,
        .maximum_schedule_lateness_microseconds =
            maximum_schedule_lateness_microseconds_,
        .last_target_qpc = last_target_qpc_,
        .output = engine_.Diagnostics(),
    };
}

WindowsAudioStreamSink::ScheduleWaitResult
WindowsAudioStreamSink::WaitForSubmissionWindow(
    const std::int64_t target_qpc,
    const std::uint64_t generation) const noexcept {
    ScheduleWaitResult result;
    const auto frequency = windows::timing::QpcClock::Frequency();
    if (target_qpc <= 0 || frequency <= 0) {
        return result;
    }
    const auto lead_ticks =
        frequency * static_cast<std::int64_t>(submission_lead_milliseconds_) / 1'000LL;
    const auto submit_qpc = target_qpc > lead_ticks ? target_qpc - lead_ticks : 0;
    const auto started_qpc = windows::timing::QpcClock::Now();
    auto now_qpc = started_qpc;
    while (now_qpc < submit_qpc) {
        if (schedule_generation_.load(std::memory_order_acquire) != generation) {
            result.valid = false;
            return result;
        }
        const auto remaining_ticks = submit_qpc - now_qpc;
        const auto remaining_seconds = static_cast<double>(remaining_ticks) /
                                       static_cast<double>(frequency);
        std::this_thread::sleep_for(std::min(std::chrono::duration<double>{remaining_seconds},
                                             std::chrono::duration<double>{0.002}));
        now_qpc = windows::timing::QpcClock::Now();
    }
    if (schedule_generation_.load(std::memory_order_acquire) != generation) {
        result.valid = false;
        return result;
    }
    if (now_qpc > started_qpc) {
        result.waited_microseconds = TicksToMicroseconds(now_qpc - started_qpc, frequency);
    }
    if (now_qpc > target_qpc) {
        result.lateness_microseconds = TicksToMicroseconds(now_qpc - target_qpc, frequency);
    }
    return result;
}

}  // namespace airplaywin::windows::audio
