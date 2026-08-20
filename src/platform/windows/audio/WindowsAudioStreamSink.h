#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include "core/audio/IAudioFrameSink.h"
#include "platform/windows/audio/WindowsAudioEngine.h"

namespace airplaywin::windows::audio {

struct AudioStreamSinkDiagnostics final {
    bool configured{false};
    bool armed{false};
    bool started{false};
    std::uint64_t accepted_frames{0U};
    std::uint64_t rejected_frames{0U};
    std::uint64_t scheduled_frames{0U};
    std::uint64_t late_scheduled_frames{0U};
    std::uint64_t scheduling_wait_microseconds{0U};
    std::uint64_t maximum_schedule_lateness_microseconds{0U};
    std::int64_t last_target_qpc{0};
    airplaywin::audio::AudioDiagnosticsSnapshot output{};
};

class WindowsAudioStreamSink final : public airplaywin::audio::IAudioFrameSink {
public:
    explicit WindowsAudioStreamSink(WasapiOutputOptions options = {},
                                    std::uint32_t prefill_milliseconds = 20U,
                                    std::uint32_t submission_lead_milliseconds = 30U);
    explicit WindowsAudioStreamSink(
        std::unique_ptr<airplaywin::audio::IAudioOutput> output,
        std::uint32_t prefill_milliseconds = 20U,
        std::uint32_t submission_lead_milliseconds = 30U);

    [[nodiscard]] bool Configure(const airplaywin::audio::AudioFormat& format) override;
    [[nodiscard]] bool Start() override;
    [[nodiscard]] bool Submit(
        const airplaywin::audio::DecodedAudioFrameView& frame) noexcept override;
    void Pause() noexcept override;
    void Resume() noexcept override;
    void Flush() noexcept override;
    void SetVolume(float linear_gain) noexcept override;
    void Stop() noexcept override;
    [[nodiscard]] airplaywin::audio::AudioSinkFeedback Feedback() const noexcept override;

    [[nodiscard]] AudioStreamSinkDiagnostics Diagnostics() const;

private:
    struct ScheduleWaitResult final {
        bool valid{true};
        std::uint64_t waited_microseconds{0U};
        std::uint64_t lateness_microseconds{0U};
    };

    [[nodiscard]] ScheduleWaitResult WaitForSubmissionWindow(
        std::int64_t target_qpc,
        std::uint64_t generation) const noexcept;

    mutable std::mutex mutex_{};
    WindowsAudioEngine engine_;
    std::uint32_t prefill_milliseconds_{20U};
    std::uint32_t submission_lead_milliseconds_{30U};
    airplaywin::audio::AudioFormat format_{};
    std::uint64_t prefilled_frames_{0U};
    std::uint64_t accepted_frames_{0U};
    std::uint64_t rejected_frames_{0U};
    std::uint64_t scheduled_frames_{0U};
    std::uint64_t late_scheduled_frames_{0U};
    std::uint64_t scheduling_wait_microseconds_{0U};
    std::uint64_t maximum_schedule_lateness_microseconds_{0U};
    std::int64_t last_target_qpc_{0};
    std::atomic<std::uint64_t> schedule_generation_{1U};
    bool configured_{false};
    bool armed_{false};
    bool started_{false};
    bool resume_after_prefill_{false};
};

}  // namespace airplaywin::windows::audio
