#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/group/EndpointCalibrationStore.h"
#include "core/group/GroupTypes.h"
#include "core/timing/RtpPtpPhaseTimeline.h"

namespace airplaywin::group {

struct GroupCoordinatorConfig final {
    std::string group_id{};
    audio::AudioFormat target_format{};
    std::uint32_t maximum_members{4U};
    std::uint32_t preroll_milliseconds{120U};
    std::uint32_t join_lead_milliseconds{250U};
    std::uint32_t presentation_boundary_frames{352U};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct GroupCoordinatorDiagnostics final {
    bool session_active{false};
    bool master_phase_pending{false};
    std::string group_id{};
    std::uint64_t session_epoch{0U};
    std::uint64_t active_master_clock_identity{0U};
    std::uint32_t member_count{0U};
    std::uint32_t active_members{0U};
    std::uint32_t holdover_members{0U};
    std::uint64_t members_added{0U};
    std::uint64_t members_removed{0U};
    std::uint64_t members_dropped{0U};
    std::uint64_t join_plans{0U};
    std::uint64_t join_activations{0U};
    std::uint64_t holdover_events{0U};
    std::uint64_t relock_events{0U};
    std::uint64_t master_change_events{0U};
    std::uint64_t calibration_updates{0U};
    std::uint64_t last_join_time_microseconds{0U};
    std::uint64_t maximum_join_time_microseconds{0U};
    std::uint64_t last_relock_time_microseconds{0U};
    std::uint64_t maximum_relock_time_microseconds{0U};
    std::uint32_t last_error{0U};
};

class GroupCoordinator final {
public:
    GroupCoordinator(
        GroupCoordinatorConfig config,
        std::shared_ptr<timing::RtpPtpPhaseTimeline> timeline,
        std::shared_ptr<IEndpointCalibrationStore> calibration_store);

    [[nodiscard]] GroupOperationResult AddMember(
        const GroupMemberDescriptor& descriptor);
    [[nodiscard]] GroupOperationResult BeginSession(
        const timing::RtpPtpPhaseAnchor& anchor);
    [[nodiscard]] GroupOperationResult ChangeMaster(
        std::uint64_t master_clock_identity) noexcept;
    [[nodiscard]] GroupOperationResult UpdateMasterPhase(
        const timing::RtpPtpPhaseAnchor& anchor) noexcept;
    [[nodiscard]] GroupOperationResult UpdateMemberTiming(
        GroupMemberId member_id,
        const GroupMemberTimingUpdate& update) noexcept;
    [[nodiscard]] std::pair<GroupOperationResult, std::optional<GroupJoinPlan>>
    PrepareJoin(GroupMemberId member_id,
                std::uint64_t remote_now_nanoseconds) noexcept;
    [[nodiscard]] std::vector<GroupMemberAction> Advance(
        std::uint64_t remote_now_nanoseconds);
    [[nodiscard]] GroupOperationResult BeginLeave(GroupMemberId member_id) noexcept;
    [[nodiscard]] GroupOperationResult CompleteLeave(GroupMemberId member_id);
    [[nodiscard]] GroupOperationResult DropMember(GroupMemberId member_id);
    [[nodiscard]] GroupOperationResult UpdateEndpointCalibration(
        GroupMemberId member_id,
        std::int64_t offset_microseconds,
        EndpointCalibrationSource source);
    void EndSession() noexcept;

    [[nodiscard]] std::shared_ptr<GroupMemberRuntime> MemberRuntime(
        GroupMemberId member_id) const noexcept;
    [[nodiscard]] std::vector<GroupMemberSnapshot> Members() const;
    [[nodiscard]] GroupCoordinatorDiagnostics Diagnostics() const;
    [[nodiscard]] std::shared_ptr<timing::RtpPtpPhaseTimeline> Timeline() const noexcept;

private:
    struct Member final {
        GroupMemberDescriptor descriptor{};
        GroupMemberState state{GroupMemberState::WaitingForSession};
        bool clock_locked{false};
        bool was_active_before_holdover{false};
        std::uint64_t clock_identity{0U};
        std::uint64_t buffered_frames{0U};
        std::uint64_t uncertainty_microseconds{0U};
        std::int64_t endpoint_latency_offset_microseconds{0};
        std::uint64_t activation_remote_ptp_nanoseconds{0U};
        std::uint64_t join_started_remote_ptp_nanoseconds{0U};
        std::uint32_t activation_rtp_timestamp{0U};
        std::shared_ptr<GroupMemberRuntime> runtime{};
    };

    void SetStateLocked(Member& member, GroupMemberState state) noexcept;
    [[nodiscard]] std::optional<GroupJoinPlan> BuildJoinPlanLocked(
        Member& member, std::uint64_t remote_now_nanoseconds) noexcept;

    GroupCoordinatorConfig config_{};
    std::shared_ptr<timing::RtpPtpPhaseTimeline> timeline_{};
    std::shared_ptr<IEndpointCalibrationStore> calibration_store_{};
    mutable std::mutex mutex_{};
    std::unordered_map<GroupMemberId, Member> members_{};
    GroupCoordinatorDiagnostics diagnostics_{};
    std::uint64_t relock_started_remote_ptp_nanoseconds_{0U};
};

}  // namespace airplaywin::group
