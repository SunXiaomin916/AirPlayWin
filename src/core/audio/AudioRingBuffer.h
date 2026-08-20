#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace airplaywin::audio {

// Fixed-capacity single-producer/single-consumer interleaved float PCM ring.
// Reset is asynchronous: the consumer commits the discard boundary before the
// producer can reuse the discarded storage.
class AudioRingBuffer final {
public:
    AudioRingBuffer(std::uint32_t capacity_frames, std::uint16_t channel_count);

    AudioRingBuffer(const AudioRingBuffer&) = delete;
    AudioRingBuffer& operator=(const AudioRingBuffer&) = delete;
    AudioRingBuffer(AudioRingBuffer&&) = delete;
    AudioRingBuffer& operator=(AudioRingBuffer&&) = delete;

    [[nodiscard]] bool Write(std::span<const float> interleaved_samples,
                             std::uint32_t frame_count) noexcept;
    [[nodiscard]] std::uint32_t Read(std::span<float> interleaved_destination,
                                     std::uint32_t max_frames) noexcept;
    [[nodiscard]] std::uint32_t AvailableFrames() const noexcept;
    [[nodiscard]] std::uint32_t FreeFrames() const noexcept;
    void Reset() noexcept;

    [[nodiscard]] std::uint32_t CapacityFrames() const noexcept { return capacity_frames_; }
    [[nodiscard]] std::uint16_t ChannelCount() const noexcept { return channel_count_; }

private:
    struct alignas(64) CacheAlignedFrameIndex final {
        std::atomic<std::uint64_t> value{0U};
        std::array<std::byte, 64U - sizeof(std::atomic<std::uint64_t>)> padding{};
    };
    static_assert(sizeof(CacheAlignedFrameIndex) == 64U);

    void ApplyPendingReset() noexcept;

    const std::uint32_t capacity_frames_;
    const std::uint16_t channel_count_;
    std::vector<float> samples_;
    std::unique_ptr<CacheAlignedFrameIndex> read_frame_;
    std::unique_ptr<CacheAlignedFrameIndex> write_frame_;
    std::atomic<std::uint64_t> discard_before_frame_{0U};
};

}  // namespace airplaywin::audio
