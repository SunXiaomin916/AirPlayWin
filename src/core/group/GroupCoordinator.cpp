#include "core/group/GroupCoordinator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace airplaywin::group {
namespace {

[[nodiscard]] std::uint64_t RequiredPrerollFrames(
    const GroupCoordinatorConfig& config) noexcept {
    return static_cast<std::uint64_t>(config.target_format.sample_rate) *
           config.preroll_milliseconds / 1'000U;
}

}  // namespace

bool GroupCoordinatorConfig::IsValid() const noexcept {
    return !group_id.empty() && group_id.size() <= 128U && target_format.IsValid() &&
           maximum_members >= 2U && maximum_members <= 4U &&
           preroll_milliseconds >= 20U && preroll_milliseconds <= 2'000U &&
           join_lead_milliseconds >= 20U && join_lead_milliseconds <= 5'000U &&
           presentation_boundary_frames >= 1U &&
           presentation_boundary_frames <= 8'192U;
}

GroupCoordinator::GroupCoordinator(
    GroupCoordinatorConfig config,
    std::shared_ptr<timing::RtpPtpPhaseTimeline> timeline,
    std::shared_ptr<IEndpointCalibrationStore> calibration_store)
    : config_(std::move(config)),
      timeline_(std::move(timeline)),
      calibration_store_(std::move(calibration_store)) {
    if (!config_.IsValid() || !timeline_ || !calibration_store_) {
        throw std::invalid_argument("invalid group coordinator configuration");
    }
    diagnostics_.group_id = config_.group_id;
}

GroupOperationResult GroupCoordinator::AddMember(
    const GroupMemberDescriptor& descriptor) {
    if (!descriptor.IsValid()) {
        return GroupOperationResult::InvalidArgument;
    }
    std::scoped_lock lock{mutex_};
    if (members_.contains(descriptor.member_id)) {
        return GroupOperationResult::DuplicateMember;
    }
    if (members_.size() >= config_.maximum_members) {
        return GroupOperationResult::GroupFull;
    }
    if (!descriptor.capabilities.Supports(config_.target_format)) {
        return GroupOperationResult::UnsupportedFormat;
    }
    Member member{
        .descriptor = descriptor,
        .state = diagnostics_.session_active ? GroupMemberState::ClockAcquiring
                                             : GroupMemberState::WaitingForSession,
        .runtime = std::make_shared<GroupMemberRuntime>(),
    };
    if (const auto calibration = calibration_store_->Load(descriptor.endpoint_id)) {
        member.endpoint_latency_offset_microseconds =
            calibration->offset_microseconds;
    }
    member.runtime->endpoint_latency_offset_microseconds_.store(
        member.endpoint_latency_offset_microseconds, std::memory_order_release);
    member.runtime->session_epoch_.store(diagnostics_.session_epoch,
                                         std::memory_order_release);
    member.runtime->UpdateState(member.state);
    members_.emplace(descriptor.member_id, std::move(member));
    ++diagnostics_.members_added;
    diagnostics_.member_count = static_cast<std::uint32_t>(members_.size());
    diagnostics_.last_error = 0U;
    return GroupOperationResult::Ok;
}

GroupOperationResult GroupCoordinator::BeginSession(
    const timing::RtpPtpPhaseAnchor& anchor) {
    if (!anchor.IsValid() || anchor.sample_rate != config_.target_format.sample_rate) {
        return GroupOperationResult::InvalidArgument;
    }
    if (!timeline_->Publish(anchor)) {
        return GroupOperationResult::TimelineUnavailable;
    }
    std::scoped_lock lock{mutex_};
    diagnostics_.session_active = true;
    diagnostics_.master_phase_pending = false;
    relock_started_remote_ptp_nanoseconds_ = 0U;
    diagnostics_.session_epoch = anchor.session_epoch;
    if (anchor.master_clock_identity != 0U) {
        if (diagnostics_.active_master_clock_identity != 0U &&
            diagnostics_.active_master_clock_identity != anchor.master_clock_identity) {
            ++diagnostics_.master_change_events;
        }
        diagnostics_.active_master_clock_identity = anchor.master_clock_identity;
    }
    for (auto& [id, member] : members_) {
        static_cast<void>(id);
        member.activation_remote_ptp_nanoseconds = 0U;
        member.activation_rtp_timestamp = 0U;
        member.runtime->session_epoch_.store(anchor.session_epoch,
                                             std::memory_order_release);
        member.runtime->activation_remote_ptp_nanoseconds_.store(
            0U, std::memory_order_release);
        member.runtime->activation_rtp_timestamp_.store(0U,
                                                        std::memory_order_release);
        const bool master_matches = diagnostics_.active_master_clock_identity == 0U ||
                                    member.clock_identity ==
                                        diagnostics_.active_master_clock_identity;
        SetStateLocked(member, member.clock_locked && master_matches
                                   ? GroupMemberState::Prerolling
                                   : GroupMemberState::ClockAcquiring);
    }
    diagnostics_.last_error = 0U;
    return GroupOperationResult::Ok;
}

GroupOperationResult GroupCoordinator::ChangeMaster(
    const std::uint64_t master_clock_identity) noexcept {
    if (master_clock_identity == 0U) {
        return GroupOperationResult::InvalidArgument;
    }
    std::scoped_lock lock{mutex_};
    if (!diagnostics_.session_active) {
        return GroupOperationResult::InvalidState;
    }
    if (diagnostics_.active_master_clock_identity == master_clock_identity) {
        return GroupOperationResult::Ok;
    }
    diagnostics_.active_master_clock_identity = master_clock_identity;
    diagnostics_.master_phase_pending = diagnostics_.session_active;
    relock_started_remote_ptp_nanoseconds_ = 0U;
    ++diagnostics_.master_change_events;
    for (auto& [id, member] : members_) {
        static_cast<void>(id);
        if (member.state == GroupMemberState::Active ||
            member.state == GroupMemberState::Holdover) {
            member.was_active_before_holdover = true;
            if (member.state != GroupMemberState::Holdover) {
                ++diagnostics_.holdover_events;
            }
            SetStateLocked(member, GroupMemberState::Holdover);
        } else if (member.state != GroupMemberState::Leaving &&
                   member.state != GroupMemberState::Dropped &&
                   member.state != GroupMemberState::Faulted) {
            SetStateLocked(member, GroupMemberState::ClockAcquiring);
        }
        member.clock_locked = false;
    }
    return GroupOperationResult::Ok;
}

GroupOperationResult GroupCoordinator::UpdateMasterPhase(
    const timing::RtpPtpPhaseAnchor& anchor) noexcept {
    std::scoped_lock lock{mutex_};
    if (!diagnostics_.session_active || !diagnostics_.master_phase_pending ||
        !anchor.IsValid() || anchor.session_epoch != diagnostics_.session_epoch ||
        anchor.sample_rate != config_.target_format.sample_rate ||
        anchor.master_clock_identity != diagnostics_.active_master_clock_identity) {
        return GroupOperationResult::InvalidArgument;
    }
    if (!timeline_->Publish(anchor)) {
        return GroupOperationResult::TimelineUnavailable;
    }
    diagnostics_.master_phase_pending = false;
    relock_started_remote_ptp_nanoseconds_ = anchor.remote_ptp_nanoseconds;
    return GroupOperationResult::Ok;
}

GroupOperationResult GroupCoordinator::UpdateMemberTiming(
    const GroupMemberId member_id,
    const GroupMemberTimingUpdate& update) noexcept {
    std::scoped_lock lock{mutex_};
    const auto found = members_.find(member_id);
    if (found == members_.end()) {
        return GroupOperationResult::MemberNotFound;
    }
    auto& member = found->second;
    if (member.state == GroupMemberState::Leaving ||
        member.state == GroupMemberState::Dropped ||
        member.state == GroupMemberState::Faulted) {
        return GroupOperationResult::InvalidState;
    }
    member.clock_identity = update.master_clock_identity;
    member.buffered_frames = update.buffered_frames;
    member.uncertainty_microseconds = update.uncertainty_microseconds;
    const bool master_matches = diagnostics_.active_master_clock_identity == 0U ||
                                update.master_clock_identity ==
                                    diagnostics_.active_master_clock_identity;
    member.clock_locked = update.clock_locked && master_matches &&
                          !diagnostics_.master_phase_pending;
    if (!member.clock_locked) {
        if (member.state == GroupMemberState::Active ||
            member.state == GroupMemberState::Holdover) {
            member.was_active_before_holdover = true;
            if (member.state != GroupMemberState::Holdover) {
                ++diagnostics_.holdover_events;
            }
            SetStateLocked(member, GroupMemberState::Holdover);
        } else {
            SetStateLocked(member, diagnostics_.session_active
                                       ? GroupMemberState::ClockAcquiring
                                       : GroupMemberState::WaitingForSession);
        }
        return update.clock_locked && master_matches &&
                       diagnostics_.master_phase_pending
                   ? GroupOperationResult::TimelineUnavailable
                   : GroupOperationResult::ClockNotLocked;
    }
    if (member.state == GroupMemberState::Holdover &&
        member.was_active_before_holdover) {
        member.was_active_before_holdover = false;
        SetStateLocked(member, GroupMemberState::Active);
        ++diagnostics_.relock_events;
        if (relock_started_remote_ptp_nanoseconds_ != 0U &&
            update.observation_remote_ptp_nanoseconds >=
                relock_started_remote_ptp_nanoseconds_) {
            const auto duration =
                (update.observation_remote_ptp_nanoseconds -
                 relock_started_remote_ptp_nanoseconds_) /
                1'000U;
            diagnostics_.last_relock_time_microseconds = duration;
            diagnostics_.maximum_relock_time_microseconds =
                std::max(diagnostics_.maximum_relock_time_microseconds, duration);
        }
    } else if (diagnostics_.session_active &&
               member.state != GroupMemberState::Active &&
               member.state != GroupMemberState::MutedReady) {
        SetStateLocked(member, GroupMemberState::Prerolling);
    }
    return GroupOperationResult::Ok;
}

std::pair<GroupOperationResult, std::optional<GroupJoinPlan>>
GroupCoordinator::PrepareJoin(const GroupMemberId member_id,
                              const std::uint64_t remote_now_nanoseconds) noexcept {
    std::scoped_lock lock{mutex_};
    const auto found = members_.find(member_id);
    if (found == members_.end()) {
        return {GroupOperationResult::MemberNotFound, std::nullopt};
    }
    auto& member = found->second;
    if (!diagnostics_.session_active ||
        member.state != GroupMemberState::Prerolling) {
        return {GroupOperationResult::InvalidState, std::nullopt};
    }
    if (!member.clock_locked) {
        return {GroupOperationResult::ClockNotLocked, std::nullopt};
    }
    if (member.buffered_frames < RequiredPrerollFrames(config_)) {
        return {GroupOperationResult::InsufficientPreroll, std::nullopt};
    }
    const auto plan = BuildJoinPlanLocked(member, remote_now_nanoseconds);
    if (!plan.has_value()) {
        return {GroupOperationResult::TimelineUnavailable, std::nullopt};
    }
    ++diagnostics_.join_plans;
    return {GroupOperationResult::Ok, plan};
}

std::vector<GroupMemberAction> GroupCoordinator::Advance(
    const std::uint64_t remote_now_nanoseconds) {
    std::scoped_lock lock{mutex_};
    std::vector<GroupMemberAction> actions;
    actions.reserve(members_.size());
    for (auto& [id, member] : members_) {
        if (member.state == GroupMemberState::MutedReady &&
            remote_now_nanoseconds >= member.activation_remote_ptp_nanoseconds) {
            SetStateLocked(member, GroupMemberState::Active);
            ++diagnostics_.join_activations;
            if (member.join_started_remote_ptp_nanoseconds != 0U &&
                remote_now_nanoseconds >=
                    member.join_started_remote_ptp_nanoseconds) {
                const auto duration =
                    (remote_now_nanoseconds -
                     member.join_started_remote_ptp_nanoseconds) /
                    1'000U;
                diagnostics_.last_join_time_microseconds = duration;
                diagnostics_.maximum_join_time_microseconds =
                    std::max(diagnostics_.maximum_join_time_microseconds, duration);
            }
            actions.push_back({.member_id = id,
                               .type = GroupMemberActionType::RampIn,
                               .session_epoch = diagnostics_.session_epoch,
                               .target_remote_ptp_nanoseconds =
                                   member.activation_remote_ptp_nanoseconds,
                               .target_rtp_timestamp = member.activation_rtp_timestamp});
        }
    }
    return actions;
}

GroupOperationResult GroupCoordinator::BeginLeave(
    const GroupMemberId member_id) noexcept {
    std::scoped_lock lock{mutex_};
    const auto found = members_.find(member_id);
    if (found == members_.end()) {
        return GroupOperationResult::MemberNotFound;
    }
    if (found->second.state == GroupMemberState::Leaving) {
        return GroupOperationResult::InvalidState;
    }
    SetStateLocked(found->second, GroupMemberState::Leaving);
    return GroupOperationResult::Ok;
}

GroupOperationResult GroupCoordinator::CompleteLeave(const GroupMemberId member_id) {
    std::scoped_lock lock{mutex_};
    const auto found = members_.find(member_id);
    if (found == members_.end()) {
        return GroupOperationResult::MemberNotFound;
    }
    if (found->second.state != GroupMemberState::Leaving) {
        return GroupOperationResult::InvalidState;
    }
    found->second.runtime->UpdateState(GroupMemberState::Dropped);
    members_.erase(found);
    ++diagnostics_.members_removed;
    diagnostics_.member_count = static_cast<std::uint32_t>(members_.size());
    return GroupOperationResult::Ok;
}

GroupOperationResult GroupCoordinator::DropMember(const GroupMemberId member_id) {
    std::scoped_lock lock{mutex_};
    const auto found = members_.find(member_id);
    if (found == members_.end()) {
        return GroupOperationResult::MemberNotFound;
    }
    found->second.runtime->UpdateState(GroupMemberState::Dropped);
    members_.erase(found);
    ++diagnostics_.members_dropped;
    diagnostics_.member_count = static_cast<std::uint32_t>(members_.size());
    return GroupOperationResult::Ok;
}

GroupOperationResult GroupCoordinator::UpdateEndpointCalibration(
    const GroupMemberId member_id,
    const std::int64_t offset_microseconds,
    const EndpointCalibrationSource source) {
    std::scoped_lock lock{mutex_};
    const auto found = members_.find(member_id);
    if (found == members_.end()) {
        return GroupOperationResult::MemberNotFound;
    }
    auto& member = found->second;
    if (member.state == GroupMemberState::Active ||
        member.state == GroupMemberState::Holdover ||
        member.state == GroupMemberState::MutedReady ||
        member.state == GroupMemberState::Leaving) {
        return GroupOperationResult::InvalidState;
    }
    const EndpointCalibration calibration{
        .endpoint_id = member.descriptor.endpoint_id,
        .offset_microseconds = offset_microseconds,
        .source = source,
    };
    if (!calibration_store_->Save(calibration)) {
        return GroupOperationResult::StorageFailure;
    }
    member.endpoint_latency_offset_microseconds = offset_microseconds;
    member.runtime->endpoint_latency_offset_microseconds_.store(
        offset_microseconds, std::memory_order_release);
    ++diagnostics_.calibration_updates;
    return GroupOperationResult::Ok;
}

void GroupCoordinator::EndSession() noexcept {
    timeline_->Clear();
    std::scoped_lock lock{mutex_};
    diagnostics_.session_active = false;
    diagnostics_.master_phase_pending = false;
    ++diagnostics_.session_epoch;
    diagnostics_.active_master_clock_identity = 0U;
    for (auto& [id, member] : members_) {
        static_cast<void>(id);
        member.activation_remote_ptp_nanoseconds = 0U;
        member.activation_rtp_timestamp = 0U;
        member.was_active_before_holdover = false;
        member.runtime->session_epoch_.store(diagnostics_.session_epoch,
                                             std::memory_order_release);
        member.runtime->activation_remote_ptp_nanoseconds_.store(
            0U, std::memory_order_release);
        member.runtime->activation_rtp_timestamp_.store(0U,
                                                        std::memory_order_release);
        SetStateLocked(member, GroupMemberState::WaitingForSession);
    }
}

std::shared_ptr<GroupMemberRuntime> GroupCoordinator::MemberRuntime(
    const GroupMemberId member_id) const noexcept {
    std::scoped_lock lock{mutex_};
    const auto found = members_.find(member_id);
    return found == members_.end() ? nullptr : found->second.runtime;
}

std::vector<GroupMemberSnapshot> GroupCoordinator::Members() const {
    std::scoped_lock lock{mutex_};
    std::vector<GroupMemberSnapshot> result;
    result.reserve(members_.size());
    for (const auto& [id, member] : members_) {
        result.push_back({
            .member_id = id,
            .endpoint_id = member.descriptor.endpoint_id,
            .state = member.state,
            .clock_locked = member.clock_locked,
            .was_active_before_holdover = member.was_active_before_holdover,
            .clock_identity = member.clock_identity,
            .buffered_frames = member.buffered_frames,
            .uncertainty_microseconds = member.uncertainty_microseconds,
            .endpoint_latency_offset_microseconds =
                member.endpoint_latency_offset_microseconds,
            .activation_remote_ptp_nanoseconds =
                member.activation_remote_ptp_nanoseconds,
            .activation_rtp_timestamp = member.activation_rtp_timestamp,
        });
    }
    std::ranges::sort(result, {}, &GroupMemberSnapshot::member_id);
    return result;
}

GroupCoordinatorDiagnostics GroupCoordinator::Diagnostics() const {
    std::scoped_lock lock{mutex_};
    auto result = diagnostics_;
    result.member_count = static_cast<std::uint32_t>(members_.size());
    result.active_members = 0U;
    result.holdover_members = 0U;
    for (const auto& [id, member] : members_) {
        static_cast<void>(id);
        result.active_members += member.state == GroupMemberState::Active ? 1U : 0U;
        result.holdover_members += member.state == GroupMemberState::Holdover ? 1U : 0U;
    }
    return result;
}

std::shared_ptr<timing::RtpPtpPhaseTimeline> GroupCoordinator::Timeline() const noexcept {
    return timeline_;
}

void GroupCoordinator::SetStateLocked(Member& member,
                                      const GroupMemberState state) noexcept {
    member.state = state;
    member.runtime->UpdateState(state);
}

std::optional<GroupJoinPlan> GroupCoordinator::BuildJoinPlanLocked(
    Member& member, const std::uint64_t remote_now_nanoseconds) noexcept {
    const auto timeline = timeline_->Diagnostics();
    if (!timeline.anchored || timeline.anchor.session_epoch != diagnostics_.session_epoch) {
        return std::nullopt;
    }
    const auto lead_nanoseconds =
        static_cast<std::uint64_t>(config_.join_lead_milliseconds) * 1'000'000U;
    if (remote_now_nanoseconds >
        std::numeric_limits<std::uint64_t>::max() - lead_nanoseconds) {
        return std::nullopt;
    }
    const auto earliest = remote_now_nanoseconds + lead_nanoseconds;
    const auto delta_nanoseconds = earliest > timeline.anchor.remote_ptp_nanoseconds
                                       ? earliest - timeline.anchor.remote_ptp_nanoseconds
                                       : 0U;
    const auto frame_position = static_cast<long double>(delta_nanoseconds) *
                                config_.target_format.sample_rate /
                                1'000'000'000.0L;
    const auto required_frames = static_cast<std::uint64_t>(std::ceil(frame_position));
    const auto boundary = static_cast<std::uint64_t>(
        config_.presentation_boundary_frames);
    if (required_frames > std::numeric_limits<std::uint64_t>::max() - (boundary - 1U)) {
        return std::nullopt;
    }
    const auto aligned_frames = (required_frames + boundary - 1U) / boundary * boundary;
    if (aligned_frames > static_cast<std::uint64_t>(
                             std::numeric_limits<std::int32_t>::max())) {
        return std::nullopt;
    }
    const auto activation_delta = aligned_frames * 1'000'000'000ULL /
                                  config_.target_format.sample_rate;
    if (timeline.anchor.remote_ptp_nanoseconds >
        std::numeric_limits<std::uint64_t>::max() - activation_delta) {
        return std::nullopt;
    }
    member.activation_remote_ptp_nanoseconds =
        timeline.anchor.remote_ptp_nanoseconds + activation_delta;
    member.join_started_remote_ptp_nanoseconds = remote_now_nanoseconds;
    member.activation_rtp_timestamp = timeline.anchor.rtp_timestamp +
                                      static_cast<std::uint32_t>(aligned_frames);
    member.runtime->activation_remote_ptp_nanoseconds_.store(
        member.activation_remote_ptp_nanoseconds, std::memory_order_release);
    member.runtime->activation_rtp_timestamp_.store(member.activation_rtp_timestamp,
                                                    std::memory_order_release);
    SetStateLocked(member, GroupMemberState::MutedReady);
    return GroupJoinPlan{
        .member_id = member.descriptor.member_id,
        .session_epoch = diagnostics_.session_epoch,
        .activation_remote_ptp_nanoseconds =
            member.activation_remote_ptp_nanoseconds,
        .activation_rtp_timestamp = member.activation_rtp_timestamp,
        .required_preroll_frames = RequiredPrerollFrames(config_),
        .endpoint_latency_offset_microseconds =
            member.endpoint_latency_offset_microseconds,
    };
}

}  // namespace airplaywin::group
