#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include "core/timing/IMonotonicClock.h"
#include "core/timing/ITimingEngine.h"

namespace airplaywin::timing {

struct BufferedRtpTimingConfig final {
    std::uint32_t remote_clock_rate{44'100U};
    std::uint32_t target_buffer_milliseconds{120U};

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return remote_clock_rate >= 8'000U && remote_clock_rate <= 384'000U &&
               target_buffer_milliseconds >= 20U && target_buffer_milliseconds <= 2'000U;
    }
};

// S7 experimental single-stream mapping. It reconstructs a buffered presentation
// timeline from RTP sample timestamps and one local QPC anchor. It is intentionally
// not a remote wall-clock/PTP servo; that belongs to the later TimingService phase.
class BufferedRtpTimingEngine final : public ITimingEngine {
public:
    BufferedRtpTimingEngine(BufferedRtpTimingConfig config,
                            std::unique_ptr<IMonotonicClock> clock);

    BufferedRtpTimingEngine(const BufferedRtpTimingEngine&) = delete;
    BufferedRtpTimingEngine& operator=(const BufferedRtpTimingEngine&) = delete;

    void Reset(std::optional<std::uint64_t> remote_time) noexcept override;
    [[nodiscard]] std::optional<std::int64_t> RemoteToLocalQpc(
        std::uint64_t remote_time) noexcept override;
    [[nodiscard]] TimingDiagnostics Diagnostics() const noexcept override;

private:
    void LockTimeline(std::uint32_t remote_timestamp, std::int64_t now_qpc) noexcept;
    [[nodiscard]] std::optional<std::int64_t> MapLocked(
        std::uint32_t remote_timestamp) noexcept;
    [[nodiscard]] std::uint64_t TicksToMicroseconds(std::int64_t ticks) const noexcept;

    BufferedRtpTimingConfig config_{};
    std::unique_ptr<IMonotonicClock> clock_{};
    std::int64_t clock_frequency_{0};
    mutable std::mutex mutex_{};
    TimingDiagnostics diagnostics_{};
};

}  // namespace airplaywin::timing
