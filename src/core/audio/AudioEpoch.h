#pragma once

#include <atomic>
#include <cstdint>

#include "core/audio/AudioTypes.h"

namespace airplaywin::audio {

class AudioEpoch final {
public:
    AudioEpoch() noexcept = default;

    [[nodiscard]] std::uint64_t Current() const noexcept;
    [[nodiscard]] AudioEpochReason LastReason() const noexcept;
    [[nodiscard]] std::uint64_t Advance(AudioEpochReason reason) noexcept;
    [[nodiscard]] bool IsCurrent(std::uint64_t epoch_id) const noexcept;

private:
    std::atomic<std::uint64_t> current_{1U};
    std::atomic<AudioEpochReason> last_reason_{AudioEpochReason::Initial};
};

}  // namespace airplaywin::audio
