#include "core/audio/AudioRingBuffer.h"

#include <algorithm>
#include <stdexcept>

namespace airplaywin::audio {

AudioRingBuffer::AudioRingBuffer(const std::uint32_t capacity_frames,
                                 const std::uint16_t channel_count)
    : capacity_frames_(capacity_frames),
      channel_count_(channel_count),
      samples_(static_cast<std::size_t>(capacity_frames) * channel_count, 0.0F),
      read_frame_(std::make_unique<CacheAlignedFrameIndex>()),
      write_frame_(std::make_unique<CacheAlignedFrameIndex>()) {
    if (capacity_frames == 0U || channel_count == 0U) {
        throw std::invalid_argument("AudioRingBuffer dimensions must be non-zero");
    }
}

bool AudioRingBuffer::Write(const std::span<const float> interleaved_samples,
                            const std::uint32_t frame_count) noexcept {
    const auto sample_count =
        static_cast<std::size_t>(frame_count) * static_cast<std::size_t>(channel_count_);
    if (frame_count == 0U || interleaved_samples.size() != sample_count) {
        return false;
    }

    const auto write = write_frame_->value.load(std::memory_order_relaxed);
    const auto read = read_frame_->value.load(std::memory_order_acquire);
    const auto used = write - read;
    if (used > capacity_frames_ || frame_count > capacity_frames_ - used) {
        return false;
    }

    const auto start_frame = static_cast<std::uint32_t>(write % capacity_frames_);
    const auto first_frames = std::min(frame_count, capacity_frames_ - start_frame);
    const auto first_samples =
        static_cast<std::size_t>(first_frames) * static_cast<std::size_t>(channel_count_);
    const auto start_sample =
        static_cast<std::size_t>(start_frame) * static_cast<std::size_t>(channel_count_);

    std::copy_n(interleaved_samples.data(), first_samples, samples_.data() + start_sample);
    const auto remaining_samples = sample_count - first_samples;
    if (remaining_samples != 0U) {
        std::copy_n(interleaved_samples.data() + first_samples, remaining_samples, samples_.data());
    }

    write_frame_->value.store(write + frame_count, std::memory_order_release);
    return true;
}

std::uint32_t AudioRingBuffer::Read(const std::span<float> interleaved_destination,
                                    const std::uint32_t max_frames) noexcept {
    if (max_frames == 0U || interleaved_destination.size() <
                                static_cast<std::size_t>(max_frames) * channel_count_) {
        return 0U;
    }

    ApplyPendingReset();
    const auto read = read_frame_->value.load(std::memory_order_relaxed);
    const auto write = write_frame_->value.load(std::memory_order_acquire);
    const auto available = std::min<std::uint64_t>(write - read, capacity_frames_);
    const auto frames = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(available, static_cast<std::uint64_t>(max_frames)));
    if (frames == 0U) {
        return 0U;
    }

    const auto start_frame = static_cast<std::uint32_t>(read % capacity_frames_);
    const auto first_frames = std::min(frames, capacity_frames_ - start_frame);
    const auto first_samples =
        static_cast<std::size_t>(first_frames) * static_cast<std::size_t>(channel_count_);
    const auto start_sample =
        static_cast<std::size_t>(start_frame) * static_cast<std::size_t>(channel_count_);

    std::copy_n(samples_.data() + start_sample, first_samples, interleaved_destination.data());
    const auto total_samples =
        static_cast<std::size_t>(frames) * static_cast<std::size_t>(channel_count_);
    const auto remaining_samples = total_samples - first_samples;
    if (remaining_samples != 0U) {
        std::copy_n(samples_.data(), remaining_samples,
                    interleaved_destination.data() + first_samples);
    }

    read_frame_->value.store(read + frames, std::memory_order_release);
    return frames;
}

std::uint32_t AudioRingBuffer::AvailableFrames() const noexcept {
    const auto read = read_frame_->value.load(std::memory_order_acquire);
    const auto discard = discard_before_frame_.load(std::memory_order_acquire);
    const auto effective_read = std::max(read, discard);
    const auto write = write_frame_->value.load(std::memory_order_acquire);
    if (write <= effective_read) {
        return 0U;
    }
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(write - effective_read, capacity_frames_));
}

std::uint32_t AudioRingBuffer::FreeFrames() const noexcept {
    const auto read = read_frame_->value.load(std::memory_order_acquire);
    const auto write = write_frame_->value.load(std::memory_order_acquire);
    const auto used = std::min<std::uint64_t>(write - read, capacity_frames_);
    return capacity_frames_ - static_cast<std::uint32_t>(used);
}

void AudioRingBuffer::Reset() noexcept {
    const auto target = write_frame_->value.load(std::memory_order_acquire);
    auto discard = discard_before_frame_.load(std::memory_order_relaxed);
    while (discard < target &&
           !discard_before_frame_.compare_exchange_weak(
               discard, target, std::memory_order_release, std::memory_order_relaxed)) {
    }
}

void AudioRingBuffer::ApplyPendingReset() noexcept {
    const auto discard = discard_before_frame_.load(std::memory_order_acquire);
    auto read = read_frame_->value.load(std::memory_order_relaxed);
    while (read < discard &&
           !read_frame_->value.compare_exchange_weak(read, discard, std::memory_order_acq_rel,
                                                     std::memory_order_relaxed)) {
    }
}

}  // namespace airplaywin::audio
