#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "core/audio/AudioTypes.h"

namespace airplaywin::group {

using GroupMemberId = std::uint64_t;

enum class GroupMemberState : std::uint8_t {
    WaitingForSession,
    ClockAcquiring,
    Prerolling,
    MutedReady,
    Active,
    Holdover,
    Leaving,
    Dropped,
    Faulted,
};

enum class GroupOperationResult : std::uint8_t {
    Ok,
    InvalidArgument,
    DuplicateMember,
    GroupFull,
    MemberNotFound,
    UnsupportedFormat,
    InvalidState,
    ClockNotLocked,
    InsufficientPreroll,
    TimelineUnavailable,
    StorageFailure,
};

enum class GroupMemberActionType : std::uint8_t {
    BeginMutedPreroll,
    RampIn,
    RampOut,
    EnterHoldover,
    Relocked,
    Removed,
};

struct GroupMemberCapabilities final {
    std::uint32_t minimum_sample_rate{8'000U};
    std::uint32_t maximum_sample_rate{384'000U};
    std::uint16_t maximum_channel_count{8U};
    bool scheduled_render{true};
    bool drift_correction{true};

    [[nodiscard]] constexpr bool Supports(
        const audio::AudioFormat& format) const noexcept {
        return format.IsValid() && format.sample_rate >= minimum_sample_rate &&
               format.sample_rate <= maximum_sample_rate &&
               format.channel_count <= maximum_channel_count && scheduled_render &&
               drift_correction;
    }
};

struct GroupMemberDescriptor final {
    GroupMemberId member_id{0U};
    std::string endpoint_id{};
    GroupMemberCapabilities capabilities{};

    [[nodiscard]] bool IsValid() const noexcept {
        return member_id != 0U && !endpoint_id.empty() && endpoint_id.size() <= 2'048U;
    }
};

struct GroupMemberTimingUpdate final {
    bool clock_locked{false};
    std::uint64_t master_clock_identity{0U};
    std::uint64_t buffered_frames{0U};
    std::uint64_t uncertainty_microseconds{0U};
    std::uint64_t observation_remote_ptp_nanoseconds{0U};
};

struct GroupJoinPlan final {
    GroupMemberId member_id{0U};
    std::uint64_t session_epoch{0U};
    std::uint64_t activation_remote_ptp_nanoseconds{0U};
    std::uint32_t activation_rtp_timestamp{0U};
    std::uint64_t required_preroll_frames{0U};
    std::int64_t endpoint_latency_offset_microseconds{0};
};

struct GroupMemberAction final {
    GroupMemberId member_id{0U};
    GroupMemberActionType type{GroupMemberActionType::BeginMutedPreroll};
    std::uint64_t session_epoch{0U};
    std::uint64_t target_remote_ptp_nanoseconds{0U};
    std::uint32_t target_rtp_timestamp{0U};
};

class GroupMemberRuntime final {
public:
    [[nodiscard]] GroupMemberState State() const noexcept;
    [[nodiscard]] std::uint64_t SessionEpoch() const noexcept;
    [[nodiscard]] std::uint32_t ActivationRtpTimestamp() const noexcept;
    [[nodiscard]] std::uint64_t ActivationRemotePtpNanoseconds() const noexcept;
    [[nodiscard]] std::int64_t EndpointLatencyOffsetMicroseconds() const noexcept;

private:
    friend class GroupCoordinator;
    void UpdateState(GroupMemberState state) noexcept;

    std::atomic<GroupMemberState> state_{GroupMemberState::WaitingForSession};
    std::atomic<std::uint64_t> session_epoch_{0U};
    std::atomic<std::uint32_t> activation_rtp_timestamp_{0U};
    std::atomic<std::uint64_t> activation_remote_ptp_nanoseconds_{0U};
    std::atomic<std::int64_t> endpoint_latency_offset_microseconds_{0};
};

struct GroupMemberSnapshot final {
    GroupMemberId member_id{0U};
    std::string endpoint_id{};
    GroupMemberState state{GroupMemberState::WaitingForSession};
    bool clock_locked{false};
    bool was_active_before_holdover{false};
    std::uint64_t clock_identity{0U};
    std::uint64_t buffered_frames{0U};
    std::uint64_t uncertainty_microseconds{0U};
    std::int64_t endpoint_latency_offset_microseconds{0};
    std::uint64_t activation_remote_ptp_nanoseconds{0U};
    std::uint32_t activation_rtp_timestamp{0U};
};

}  // namespace airplaywin::group
