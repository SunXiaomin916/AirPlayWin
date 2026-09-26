#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

namespace airplaywin::timing {

struct RtpPtpPhaseAnchor final {
    std::uint32_t rtp_timestamp{0U};
    std::uint64_t remote_ptp_nanoseconds{0U};
    std::uint32_t sample_rate{0U};
    std::uint64_t session_epoch{0U};
    std::uint64_t master_clock_identity{0U};

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return remote_ptp_nanoseconds != 0U && sample_rate >= 8'000U &&
               sample_rate <= 384'000U && session_epoch != 0U;
    }
};

struct RtpPtpPhaseDiagnostics final {
    bool anchored{false};
    std::uint64_t generation{0U};
    std::uint64_t published_anchors{0U};
    std::uint64_t cleared_anchors{0U};
    std::uint64_t mapped_timestamps{0U};
    std::uint64_t rejected_mappings{0U};
    RtpPtpPhaseAnchor anchor{};
};

class RtpPtpPhaseTimeline final {
public:
    [[nodiscard]] bool Publish(const RtpPtpPhaseAnchor& anchor) noexcept;
    void Clear() noexcept;
    [[nodiscard]] std::optional<std::uint64_t> RtpToRemotePtpNanoseconds(
        std::uint32_t rtp_timestamp) const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> RemotePtpNanosecondsToRtp(
        std::uint64_t remote_ptp_nanoseconds) const noexcept;
    [[nodiscard]] RtpPtpPhaseDiagnostics Diagnostics() const noexcept;

private:
    mutable std::mutex mutex_{};
    mutable RtpPtpPhaseDiagnostics diagnostics_{};
};

}  // namespace airplaywin::timing
