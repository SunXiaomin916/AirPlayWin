#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace airplaywin::audio {

struct ClickPopDetectorConfig final {
    float maximum_sample_step{0.35F};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct ClickPopDiagnostics final {
    std::uint64_t analyzed_frames{0U};
    std::uint64_t transient_events{0U};
    std::uint64_t last_event_frame{0U};
    float maximum_sample_step{0.0F};
    float recent_peak_step{0.0F};
};

// Fixed-storage waveform discontinuity detector. Process() is intended for one
// real-time render thread; diagnostics are published atomically for readers.
class ClickPopDetector final {
public:
    static constexpr std::size_t kMaximumChannels = 8U;

    explicit ClickPopDetector(ClickPopDetectorConfig config = {});

    void Process(std::span<const float> interleaved_samples,
                 std::uint32_t frame_count,
                 std::uint16_t channel_count) noexcept;
    void Reset() noexcept;

    [[nodiscard]] ClickPopDiagnostics Diagnostics() const noexcept;

private:
    [[nodiscard]] static std::uint32_t FloatBits(float value) noexcept;
    [[nodiscard]] static float BitsFloat(std::uint32_t value) noexcept;
    static void StoreMaximum(std::atomic<std::uint32_t>& destination,
                             float value) noexcept;

    ClickPopDetectorConfig config_{};
    std::array<float, kMaximumChannels> previous_samples_{};
    std::array<bool, kMaximumChannels> have_previous_sample_{};
    std::uint64_t frame_cursor_{0U};

    std::atomic<std::uint64_t> analyzed_frames_{0U};
    std::atomic<std::uint64_t> transient_events_{0U};
    std::atomic<std::uint64_t> last_event_frame_{0U};
    std::atomic<std::uint32_t> maximum_sample_step_bits_{0U};
    std::atomic<std::uint32_t> recent_peak_step_bits_{0U};
};

}  // namespace airplaywin::audio
