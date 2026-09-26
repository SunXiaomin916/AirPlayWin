#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>

#include "core/timing/ClockServo.h"
#include "core/timing/PtpPacket.h"

namespace airplaywin::timing {

struct PtpClockDomainDiagnostics final {
    std::uint64_t received_datagrams{0U};
    std::uint64_t invalid_datagrams{0U};
    std::uint64_t sync_messages{0U};
    std::uint64_t follow_up_messages{0U};
    std::uint64_t announce_messages{0U};
    std::uint64_t ignored_messages{0U};
    std::uint64_t unmatched_follow_up_messages{0U};
    std::uint64_t completed_samples{0U};
    std::uint8_t domain_number{0U};
    ClockServoDiagnostics servo{};
};

class PtpClockDomain final {
public:
    explicit PtpClockDomain(ClockServoConfig config);

    PtpClockDomain(const PtpClockDomain&) = delete;
    PtpClockDomain& operator=(const PtpClockDomain&) = delete;

    void OnDatagram(std::span<const std::byte> datagram,
                    std::int64_t local_receive_qpc) noexcept;
    void Update(std::int64_t now_qpc) noexcept;
    void Reset() noexcept;
    [[nodiscard]] std::optional<std::int64_t> RemoteToLocalQpc(
        std::uint64_t remote_time_nanoseconds) const noexcept;
    [[nodiscard]] double RateCorrection() const noexcept;
    [[nodiscard]] bool ConsumeHardResyncRequest() noexcept;
    [[nodiscard]] PtpClockDomainDiagnostics Diagnostics() const noexcept;

private:
    struct PendingSync final {
        bool valid{false};
        std::uint8_t domain_number{0U};
        std::uint64_t source_clock_identity{0U};
        std::uint16_t source_port_number{0U};
        std::uint16_t sequence_id{0U};
        std::int64_t correction_nanoseconds{0};
        std::int64_t local_receive_qpc{0};
    };

    [[nodiscard]] static std::optional<std::uint64_t> ApplyCorrection(
        std::uint64_t timestamp_nanoseconds,
        std::int64_t correction_nanoseconds) noexcept;
    void SubmitSampleLocked(std::uint64_t remote_time_nanoseconds,
                            std::int64_t local_receive_qpc,
                            std::uint64_t source_clock_identity) noexcept;

    mutable std::mutex mutex_{};
    ClockServo servo_;
    PendingSync pending_{};
    PtpClockDomainDiagnostics diagnostics_{};
};

}  // namespace airplaywin::timing
