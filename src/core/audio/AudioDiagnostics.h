#pragma once

#include <atomic>
#include <cstdint>

#include "core/audio/AudioTypes.h"

namespace airplaywin::audio {

class AudioDiagnostics final {
public:
    void RecordUnderrun() noexcept { underrun_count_.fetch_add(1U, std::memory_order_relaxed); }
    void RecordRenderedFrames(const std::uint32_t frames) noexcept {
        rendered_frames_.fetch_add(frames, std::memory_order_relaxed);
    }
    void RecordStaleEpochBuffer() noexcept {
        dropped_stale_epoch_buffers_.fetch_add(1U, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t UnderrunCount() const noexcept {
        return underrun_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t RenderedFrames() const noexcept {
        return rendered_frames_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t DroppedStaleEpochBuffers() const noexcept {
        return dropped_stale_epoch_buffers_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> underrun_count_{0};
    std::atomic<std::uint64_t> rendered_frames_{0};
    std::atomic<std::uint64_t> dropped_stale_epoch_buffers_{0};
};

}  // namespace airplaywin::audio
