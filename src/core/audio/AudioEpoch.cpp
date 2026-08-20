#include "core/audio/AudioEpoch.h"

namespace airplaywin::audio {

std::uint64_t AudioEpoch::Current() const noexcept {
    return current_.load(std::memory_order_acquire);
}

AudioEpochReason AudioEpoch::LastReason() const noexcept {
    return last_reason_.load(std::memory_order_acquire);
}

std::uint64_t AudioEpoch::Advance(const AudioEpochReason reason) noexcept {
    last_reason_.store(reason, std::memory_order_release);
    return current_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
}

bool AudioEpoch::IsCurrent(const std::uint64_t epoch_id) const noexcept {
    return epoch_id == Current();
}

}  // namespace airplaywin::audio
