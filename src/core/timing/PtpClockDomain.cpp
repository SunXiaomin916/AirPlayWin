#include "core/timing/PtpClockDomain.h"

#include <limits>

namespace airplaywin::timing {
namespace {

[[nodiscard]] std::optional<std::int64_t> CombineCorrections(
    const std::int64_t left, const std::int64_t right) noexcept {
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
        return std::nullopt;
    }
    return left + right;
}

}  // namespace

PtpClockDomain::PtpClockDomain(const ClockServoConfig config) : servo_(config) {}

void PtpClockDomain::OnDatagram(const std::span<const std::byte> datagram,
                                const std::int64_t local_receive_qpc) noexcept {
    std::scoped_lock lock{mutex_};
    ++diagnostics_.received_datagrams;
    const auto packet = ParsePtpPacket(datagram);
    if (!packet.has_value() || local_receive_qpc <= 0) {
        ++diagnostics_.invalid_datagrams;
        return;
    }
    diagnostics_.domain_number = packet->domain_number;
    switch (packet->message_type) {
    case PtpMessageType::Sync: {
        ++diagnostics_.sync_messages;
        if (packet->two_step) {
            pending_ = PendingSync{
                .valid = true,
                .domain_number = packet->domain_number,
                .source_clock_identity = packet->source_clock_identity,
                .source_port_number = packet->source_port_number,
                .sequence_id = packet->sequence_id,
                .correction_nanoseconds = packet->correction_nanoseconds,
                .local_receive_qpc = local_receive_qpc,
            };
            return;
        }
        const auto remote = ApplyCorrection(*packet->origin_timestamp_nanoseconds,
                                            packet->correction_nanoseconds);
        if (!remote.has_value()) {
            ++diagnostics_.invalid_datagrams;
            return;
        }
        SubmitSampleLocked(*remote, local_receive_qpc, packet->source_clock_identity);
        return;
    }
    case PtpMessageType::FollowUp: {
        ++diagnostics_.follow_up_messages;
        if (!pending_.valid || pending_.domain_number != packet->domain_number ||
            pending_.source_clock_identity != packet->source_clock_identity ||
            pending_.source_port_number != packet->source_port_number ||
            pending_.sequence_id != packet->sequence_id) {
            ++diagnostics_.unmatched_follow_up_messages;
            return;
        }
        const auto combined_correction = CombineCorrections(
            pending_.correction_nanoseconds, packet->correction_nanoseconds);
        if (!combined_correction.has_value()) {
            ++diagnostics_.invalid_datagrams;
            pending_.valid = false;
            return;
        }
        const auto remote = ApplyCorrection(*packet->origin_timestamp_nanoseconds,
                                            *combined_correction);
        if (!remote.has_value()) {
            ++diagnostics_.invalid_datagrams;
            pending_.valid = false;
            return;
        }
        SubmitSampleLocked(*remote, pending_.local_receive_qpc,
                           pending_.source_clock_identity);
        pending_.valid = false;
        return;
    }
    case PtpMessageType::Announce:
        ++diagnostics_.announce_messages;
        return;
    default:
        ++diagnostics_.ignored_messages;
        return;
    }
}

void PtpClockDomain::Update(const std::int64_t now_qpc) noexcept {
    servo_.Update(now_qpc);
}

void PtpClockDomain::Reset() noexcept {
    std::scoped_lock lock{mutex_};
    pending_ = {};
    servo_.Reset();
}

std::optional<std::int64_t> PtpClockDomain::RemoteToLocalQpc(
    const std::uint64_t remote_time_nanoseconds) const noexcept {
    return servo_.RemoteToLocalQpc(remote_time_nanoseconds);
}

double PtpClockDomain::RateCorrection() const noexcept {
    return servo_.RateCorrection();
}

bool PtpClockDomain::ConsumeHardResyncRequest() noexcept {
    return servo_.ConsumeHardResyncRequest();
}

PtpClockDomainDiagnostics PtpClockDomain::Diagnostics() const noexcept {
    std::scoped_lock lock{mutex_};
    auto result = diagnostics_;
    result.servo = servo_.Diagnostics();
    return result;
}

std::optional<std::uint64_t> PtpClockDomain::ApplyCorrection(
    const std::uint64_t timestamp_nanoseconds,
    const std::int64_t correction_nanoseconds) noexcept {
    if (correction_nanoseconds >= 0) {
        const auto correction = static_cast<std::uint64_t>(correction_nanoseconds);
        if (timestamp_nanoseconds > std::numeric_limits<std::uint64_t>::max() - correction) {
            return std::nullopt;
        }
        return timestamp_nanoseconds + correction;
    }
    const auto magnitude = static_cast<std::uint64_t>(-(correction_nanoseconds + 1LL)) + 1U;
    return magnitude > timestamp_nanoseconds
               ? std::nullopt
               : std::optional<std::uint64_t>{timestamp_nanoseconds - magnitude};
}

void PtpClockDomain::SubmitSampleLocked(const std::uint64_t remote_time_nanoseconds,
                                        const std::int64_t local_receive_qpc,
                                        const std::uint64_t source_clock_identity) noexcept {
    if (servo_.AddSample({.remote_time_nanoseconds = remote_time_nanoseconds,
                          .local_receive_qpc = local_receive_qpc,
                          .source_clock_identity = source_clock_identity})) {
        ++diagnostics_.completed_samples;
    }
}

}  // namespace airplaywin::timing
