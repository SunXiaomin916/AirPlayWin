#include "core/group/GroupTypes.h"

namespace airplaywin::group {

GroupMemberState GroupMemberRuntime::State() const noexcept {
    return state_.load(std::memory_order_acquire);
}

std::uint64_t GroupMemberRuntime::SessionEpoch() const noexcept {
    return session_epoch_.load(std::memory_order_acquire);
}

std::uint32_t GroupMemberRuntime::ActivationRtpTimestamp() const noexcept {
    return activation_rtp_timestamp_.load(std::memory_order_acquire);
}

std::uint64_t GroupMemberRuntime::ActivationRemotePtpNanoseconds() const noexcept {
    return activation_remote_ptp_nanoseconds_.load(std::memory_order_acquire);
}

std::int64_t GroupMemberRuntime::EndpointLatencyOffsetMicroseconds() const noexcept {
    return endpoint_latency_offset_microseconds_.load(std::memory_order_acquire);
}

void GroupMemberRuntime::UpdateState(const GroupMemberState state) noexcept {
    state_.store(state, std::memory_order_release);
}

}  // namespace airplaywin::group
