#include "TestFramework.h"

#include <array>
#include <memory>

#include "FakeAudioFrameSink.h"
#include "core/group/EndpointCalibrationStore.h"
#include "core/group/GroupCoordinator.h"
#include "core/group/GroupMemberAudioGate.h"

void TestGroupMemberAudioGate() {
    using airplaywin::audio::DecodedAudioFrameView;
    using airplaywin::group::GroupCoordinator;
    using airplaywin::group::GroupCoordinatorConfig;
    using airplaywin::group::GroupMemberAudioGate;
    using airplaywin::group::GroupMemberTimingUpdate;
    using airplaywin::group::GroupOperationResult;
    using airplaywin::group::InMemoryEndpointCalibrationStore;
    using airplaywin::tests::FakeAudioFrameSink;
    using airplaywin::timing::RtpPtpPhaseAnchor;
    using airplaywin::timing::RtpPtpPhaseTimeline;

    auto timeline = std::make_shared<RtpPtpPhaseTimeline>();
    auto calibration = std::make_shared<InMemoryEndpointCalibrationStore>();
    GroupCoordinator coordinator{
        GroupCoordinatorConfig{.group_id = "gate-test",
                               .target_format = {.sample_rate = 48'000U,
                                                 .channel_count = 2U},
                               .preroll_milliseconds = 20U,
                               .join_lead_milliseconds = 20U,
                               .presentation_boundary_frames = 480U},
        timeline, calibration};
    APW_EXPECT(coordinator.AddMember({.member_id = 1U, .endpoint_id = "speaker-1"}) ==
               GroupOperationResult::Ok);
    const auto runtime = coordinator.MemberRuntime(1U);
    APW_EXPECT(runtime != nullptr);

    FakeAudioFrameSink sink;
    GroupMemberAudioGate gate{sink, runtime};
    APW_EXPECT(gate.Configure({.sample_rate = 48'000U, .channel_count = 2U}));
    gate.SetVolume(0.75F);
    APW_EXPECT(gate.Start());

    std::array<float, 8U> samples{0.25F, -0.25F, 0.5F, -0.5F,
                                 0.75F, -0.75F, 1.0F, -1.0F};
    auto SubmitAt = [&](const std::uint32_t timestamp) {
        return gate.Submit(DecodedAudioFrameView{
            .interleaved_samples = samples,
            .frame_count = 4U,
            .rtp_timestamp = timestamp,
        });
    };

    APW_EXPECT(SubmitAt(100U));
    const auto pre_session = sink.Samples();
    APW_EXPECT(pre_session.size() == samples.size());
    for (const auto sample : pre_session) {
        APW_EXPECT(sample == 0.0F);
    }

    constexpr std::uint64_t remote_base = 1'700'000'000'000'000'000ULL;
    APW_EXPECT(coordinator.BeginSession(RtpPtpPhaseAnchor{
                   .rtp_timestamp = 1'000U,
                   .remote_ptp_nanoseconds = remote_base,
                   .sample_rate = 48'000U,
                   .session_epoch = 3U,
                   .master_clock_identity = 9U,
               }) == GroupOperationResult::Ok);
    APW_EXPECT(coordinator.UpdateMemberTiming(
                   1U, GroupMemberTimingUpdate{.clock_locked = true,
                                               .master_clock_identity = 9U,
                                               .buffered_frames = 960U}) ==
               GroupOperationResult::Ok);
    const auto [result, plan] = coordinator.PrepareJoin(1U, remote_base);
    APW_EXPECT(result == GroupOperationResult::Ok);
    APW_EXPECT(plan.has_value());

    APW_EXPECT(SubmitAt(plan->activation_rtp_timestamp - 4U));
    APW_EXPECT(sink.Volume() == 0.0F);
    APW_EXPECT(SubmitAt(plan->activation_rtp_timestamp));
    APW_EXPECT(sink.Volume() == 0.75F);
    auto diagnostics = gate.Diagnostics();
    APW_EXPECT(diagnostics.muted_frames == 8U);
    APW_EXPECT(diagnostics.audible_frames == 4U);
    APW_EXPECT(diagnostics.ramp_in_events == 1U);

    APW_EXPECT(coordinator.Advance(plan->activation_remote_ptp_nanoseconds).size() == 1U);
    APW_EXPECT(coordinator.ChangeMaster(10U) == GroupOperationResult::Ok);
    APW_EXPECT(SubmitAt(plan->activation_rtp_timestamp + 4U));
    APW_EXPECT(sink.Volume() == 0.75F);

    APW_EXPECT(coordinator.BeginLeave(1U) == GroupOperationResult::Ok);
    APW_EXPECT(SubmitAt(plan->activation_rtp_timestamp + 8U));
    APW_EXPECT(sink.Volume() == 0.0F);
    diagnostics = gate.Diagnostics();
    APW_EXPECT(diagnostics.ramp_out_events == 1U);
    APW_EXPECT(diagnostics.muted_frames == 12U);

    gate.Stop();
}
