#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include "core/timing/IMonotonicClock.h"
#include "core/timing/ITimingEngine.h"
#include "core/timing/PtpClockDomain.h"

namespace airplaywin::timing {

struct DisciplinedRtpTimingConfig final {
    std::uint32_t remote_clock_rate{44'100U};
    std::uint32_t target_buffer_milliseconds{120U};

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return remote_clock_rate >= 8'000U && remote_clock_rate <= 384'000U &&
               target_buffer_milliseconds >= 20U && target_buffer_milliseconds <= 2'000U;
    }
};

// Maps an RTP sample timeline to local QPC while applying frequency discipline from a
// shared PTP clock domain. Absolute RTP-to-PTP phase alignment is intentionally deferred
// to the group/session anchor supplied by the S11 coordinator.
class DisciplinedRtpTimingEngine final : public ITimingEngine {
public:
    DisciplinedRtpTimingEngine(
        DisciplinedRtpTimingConfig config,
        std::shared_ptr<PtpClockDomain> clock_domain,
        std::unique_ptr<IMonotonicClock> clock);

    void Reset(std::optional<std::uint64_t> remote_time) noexcept override;
    [[nodiscard]] std::optional<std::int64_t> RemoteToLocalQpc(
        std::uint64_t remote_time) noexcept override;
    [[nodiscard]] TimingDiagnostics Diagnostics() const noexcept override;
    [[nodiscard]] double RateCorrection() const noexcept override;
    [[nodiscard]] bool ConsumeHardResyncRequest() noexcept override;

private:
    void LockTimeline(std::uint32_t remote_timestamp, std::int64_t now_qpc) noexcept;
    [[nodiscard]] std::optional<std::int64_t> MapLocked(
        std::uint32_t remote_timestamp,
        double rate_correction) noexcept;
    [[nodiscard]] std::uint64_t TicksToMicroseconds(std::int64_t ticks) const noexcept;

    DisciplinedRtpTimingConfig config_{};
    std::shared_ptr<PtpClockDomain> clock_domain_{};
    std::unique_ptr<IMonotonicClock> clock_{};
    std::int64_t clock_frequency_{0};
    mutable std::mutex mutex_{};
    TimingDiagnostics diagnostics_{};
};

}  // namespace airplaywin::timing
