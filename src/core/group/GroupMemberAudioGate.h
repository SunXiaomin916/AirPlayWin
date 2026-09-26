#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/audio/IAudioFrameSink.h"
#include "core/group/GroupTypes.h"

namespace airplaywin::group {

struct GroupMemberAudioGateDiagnostics final {
    bool configured{false};
    bool audible{false};
    std::uint64_t muted_frames{0U};
    std::uint64_t audible_frames{0U};
    std::uint64_t rejected_frames{0U};
    std::uint64_t ramp_in_events{0U};
    std::uint64_t ramp_out_events{0U};
};

// Keeps a member silent while its clock and pre-roll converge. The wrapped sink remains the
// only path to the platform audio implementation, so AudioTransitionGuard still owns the
// actual sample ramp and click/pop protection.
class GroupMemberAudioGate final : public audio::IAudioFrameSink {
public:
    GroupMemberAudioGate(audio::IAudioFrameSink& sink,
                         std::shared_ptr<GroupMemberRuntime> runtime);

    [[nodiscard]] bool Configure(const audio::AudioFormat& format) override;
    [[nodiscard]] bool Start() override;
    [[nodiscard]] bool Submit(const audio::DecodedAudioFrameView& frame) noexcept override;
    void Pause() noexcept override;
    void Resume() noexcept override;
    void Flush() noexcept override;
    void HardResync() noexcept override;
    void SetVolume(float linear_gain) noexcept override;
    void Stop() noexcept override;
    [[nodiscard]] audio::AudioSinkFeedback Feedback() const noexcept override;

    [[nodiscard]] GroupMemberAudioGateDiagnostics Diagnostics() const noexcept;

private:
    static constexpr std::uint32_t kMaximumFrameCount = 8'192U;

    [[nodiscard]] bool ShouldRenderAudibly(
        const audio::DecodedAudioFrameView& frame) const noexcept;
    void ApplyAudibleState(bool audible) noexcept;

    audio::IAudioFrameSink& sink_;
    std::shared_ptr<GroupMemberRuntime> runtime_{};
    std::vector<float> silence_{};
    std::atomic<float> desired_gain_{1.0F};
    std::atomic<bool> configured_{false};
    std::atomic<bool> audible_{false};
    std::atomic<std::uint16_t> channel_count_{0U};
    std::atomic<std::uint64_t> muted_frames_{0U};
    std::atomic<std::uint64_t> audible_frames_{0U};
    std::atomic<std::uint64_t> rejected_frames_{0U};
    std::atomic<std::uint64_t> ramp_in_events_{0U};
    std::atomic<std::uint64_t> ramp_out_events_{0U};
};

}  // namespace airplaywin::group
