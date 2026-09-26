#include "TestFramework.h"

#include <memory>

#include "core/group/EndpointCalibrationStore.h"
#include "core/group/GroupCoordinator.h"

void TestGroupCoordinator() {
    using airplaywin::group::EndpointCalibrationSource;
    using airplaywin::group::GroupCoordinator;
    using airplaywin::group::GroupCoordinatorConfig;
    using airplaywin::group::GroupMemberDescriptor;
    using airplaywin::group::GroupMemberState;
    using airplaywin::group::GroupMemberTimingUpdate;
    using airplaywin::group::GroupOperationResult;
    using airplaywin::group::InMemoryEndpointCalibrationStore;
    using airplaywin::timing::RtpPtpPhaseAnchor;
    using airplaywin::timing::RtpPtpPhaseTimeline;

    auto calibration = std::make_shared<InMemoryEndpointCalibrationStore>();
    APW_EXPECT(calibration->Save({.endpoint_id = "usb-1",
                                  .offset_microseconds = 1'500,
                                  .source = EndpointCalibrationSource::Measured}));
    auto timeline = std::make_shared<RtpPtpPhaseTimeline>();
    GroupCoordinator coordinator{
        GroupCoordinatorConfig{
            .group_id = "living-room",
            .target_format = {.sample_rate = 48'000U, .channel_count = 2U},
            .maximum_members = 4U,
            .preroll_milliseconds = 100U,
            .join_lead_milliseconds = 250U,
            .presentation_boundary_frames = 480U,
        },
        timeline, calibration};

    APW_EXPECT(coordinator.AddMember({.member_id = 1U, .endpoint_id = "usb-1"}) ==
               GroupOperationResult::Ok);
    APW_EXPECT(coordinator.AddMember({.member_id = 2U, .endpoint_id = "hdmi-2"}) ==
               GroupOperationResult::Ok);
    APW_EXPECT(coordinator.AddMember({.member_id = 3U, .endpoint_id = "realtek-3"}) ==
               GroupOperationResult::Ok);
    APW_EXPECT(coordinator.AddMember({.member_id = 4U, .endpoint_id = "dac-4"}) ==
               GroupOperationResult::Ok);
    APW_EXPECT(coordinator.AddMember({.member_id = 4U, .endpoint_id = "duplicate"}) ==
               GroupOperationResult::DuplicateMember);
    APW_EXPECT(coordinator.AddMember({.member_id = 5U, .endpoint_id = "too-many"}) ==
               GroupOperationResult::GroupFull);

    constexpr std::uint64_t remote_base = 1'700'000'000'000'000'000ULL;
    APW_EXPECT(coordinator.BeginSession(RtpPtpPhaseAnchor{
                   .rtp_timestamp = 1'000U,
                   .remote_ptp_nanoseconds = remote_base,
                   .sample_rate = 48'000U,
                   .session_epoch = 7U,
                   .master_clock_identity = 42U,
               }) == GroupOperationResult::Ok);
    for (const auto member_id : {1U, 2U}) {
        APW_EXPECT(coordinator.UpdateMemberTiming(
                       member_id,
                       GroupMemberTimingUpdate{.clock_locked = true,
                                               .master_clock_identity = 42U,
                                               .buffered_frames = 4'800U,
                                               .uncertainty_microseconds = 250U}) ==
                   GroupOperationResult::Ok);
    }
    const auto [first_result, first_plan] =
        coordinator.PrepareJoin(1U, remote_base + 1'000'000'000ULL);
    APW_EXPECT(first_result == GroupOperationResult::Ok);
    APW_EXPECT(first_plan.has_value());
    APW_EXPECT(first_plan->activation_remote_ptp_nanoseconds ==
               remote_base + 1'250'000'000ULL);
    APW_EXPECT(first_plan->activation_rtp_timestamp == 61'000U);
    APW_EXPECT(first_plan->endpoint_latency_offset_microseconds == 1'500);

    const auto [second_result, second_plan] =
        coordinator.PrepareJoin(2U, remote_base + 1'000'000'000ULL);
    APW_EXPECT(second_result == GroupOperationResult::Ok);
    APW_EXPECT(second_plan.has_value());
    APW_EXPECT(second_plan->activation_rtp_timestamp ==
               first_plan->activation_rtp_timestamp);
    APW_EXPECT(coordinator.Advance(first_plan->activation_remote_ptp_nanoseconds - 1U)
                   .empty());
    const auto activated =
        coordinator.Advance(first_plan->activation_remote_ptp_nanoseconds);
    APW_EXPECT(activated.size() == 2U);
    APW_EXPECT(coordinator.Diagnostics().active_members == 2U);

    APW_EXPECT(coordinator.UpdateMemberTiming(
                   3U, GroupMemberTimingUpdate{.clock_locked = true,
                                               .master_clock_identity = 42U,
                                               .buffered_frames = 4'800U}) ==
               GroupOperationResult::Ok);
    const auto [late_result, late_plan] = coordinator.PrepareJoin(
        3U, first_plan->activation_remote_ptp_nanoseconds + 1'000'000'000ULL);
    APW_EXPECT(late_result == GroupOperationResult::Ok);
    APW_EXPECT(late_plan.has_value());
    APW_EXPECT(late_plan->activation_remote_ptp_nanoseconds >
               first_plan->activation_remote_ptp_nanoseconds);
    APW_EXPECT(coordinator.Diagnostics().active_members == 2U);

    const auto epoch_before_leave = coordinator.Diagnostics().session_epoch;
    APW_EXPECT(coordinator.BeginLeave(1U) == GroupOperationResult::Ok);
    APW_EXPECT(coordinator.CompleteLeave(1U) == GroupOperationResult::Ok);
    APW_EXPECT(coordinator.Diagnostics().session_epoch == epoch_before_leave);
    APW_EXPECT(coordinator.Diagnostics().active_members == 1U);

    APW_EXPECT(coordinator.ChangeMaster(99U) == GroupOperationResult::Ok);
    APW_EXPECT(coordinator.Diagnostics().holdover_members == 1U);
    APW_EXPECT(coordinator.UpdateMemberTiming(
                   2U, GroupMemberTimingUpdate{.clock_locked = true,
                                               .master_clock_identity = 42U,
                                               .buffered_frames = 4'800U}) ==
               GroupOperationResult::ClockNotLocked);
    APW_EXPECT(coordinator.UpdateMemberTiming(
                   2U, GroupMemberTimingUpdate{.clock_locked = true,
                                               .master_clock_identity = 99U,
                                               .buffered_frames = 4'800U}) ==
               GroupOperationResult::TimelineUnavailable);
    APW_EXPECT(coordinator.Diagnostics().holdover_members == 1U);
    APW_EXPECT(coordinator.UpdateMasterPhase(RtpPtpPhaseAnchor{
                   .rtp_timestamp = 73'000U,
                   .remote_ptp_nanoseconds = remote_base + 1'500'000'000ULL,
                   .sample_rate = 48'000U,
                   .session_epoch = 7U,
                   .master_clock_identity = 99U,
               }) == GroupOperationResult::Ok);
    APW_EXPECT(coordinator.UpdateMemberTiming(
                   2U, GroupMemberTimingUpdate{.clock_locked = true,
                                               .master_clock_identity = 99U,
                                               .buffered_frames = 4'800U}) ==
               GroupOperationResult::Ok);
    APW_EXPECT(coordinator.Diagnostics().active_members == 1U);
    APW_EXPECT(coordinator.Diagnostics().relock_events == 1U);

    APW_EXPECT(coordinator.UpdateEndpointCalibration(
                   2U, 2'000, EndpointCalibrationSource::Manual) ==
               GroupOperationResult::InvalidState);
    APW_EXPECT(coordinator.UpdateEndpointCalibration(
                   4U, -500, EndpointCalibrationSource::Manual) ==
               GroupOperationResult::Ok);
    APW_EXPECT(calibration->Load("dac-4")->offset_microseconds == -500);
    APW_EXPECT(coordinator.DropMember(3U) == GroupOperationResult::Ok);

    for (std::uint64_t cycle = 0U; cycle < 100U; ++cycle) {
        const auto id = 100U + cycle;
        APW_EXPECT(coordinator.AddMember(GroupMemberDescriptor{
                       .member_id = id,
                       .endpoint_id = "cycle-" + std::to_string(cycle)}) ==
                   GroupOperationResult::Ok);
        APW_EXPECT(coordinator.BeginLeave(id) == GroupOperationResult::Ok);
        APW_EXPECT(coordinator.CompleteLeave(id) == GroupOperationResult::Ok);
        APW_EXPECT(coordinator.Diagnostics().active_members == 1U);
    }
    coordinator.EndSession();
    APW_EXPECT(!coordinator.Diagnostics().session_active);
    APW_EXPECT(!timeline->Diagnostics().anchored);
}
