#include "core/audio/ClickPopDetector.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace airplaywin::audio {

bool ClickPopDetectorConfig::IsValid() const noexcept {
    return std::isfinite(maximum_sample_step) && maximum_sample_step > 0.0F &&
           maximum_sample_step <= 2.0F;
}

ClickPopDetector::ClickPopDetector(const ClickPopDetectorConfig config) : config_(config) {
    if (!config_.IsValid()) {
        throw std::invalid_argument("invalid click/pop detector configuration");
    }
}

void ClickPopDetector::Process(const std::span<const float> interleaved_samples,
                               const std::uint32_t frame_count,
                               const std::uint16_t channel_count) noexcept {
    const auto channels = static_cast<std::size_t>(channel_count);
    const auto required_samples = static_cast<std::size_t>(frame_count) * channels;
    if (frame_count == 0U || channel_count == 0U || channels > kMaximumChannels ||
        interleaved_samples.size() < required_samples) {
        return;
    }

    float block_peak_step = 0.0F;
    std::uint64_t block_events = 0U;
    std::uint64_t last_event_frame = 0U;
    for (std::uint32_t frame = 0U; frame < frame_count; ++frame) {
        bool frame_has_transient = false;
        for (std::size_t channel = 0U; channel < channels; ++channel) {
            const auto index = static_cast<std::size_t>(frame) * channels + channel;
            const auto sample = std::isfinite(interleaved_samples[index])
                                    ? interleaved_samples[index]
                                    : 0.0F;
            if (have_previous_sample_[channel]) {
                const auto step = std::abs(sample - previous_samples_[channel]);
                block_peak_step = std::max(block_peak_step, step);
                frame_has_transient = frame_has_transient ||
                                      step > config_.maximum_sample_step;
            }
            previous_samples_[channel] = sample;
            have_previous_sample_[channel] = true;
        }
        if (frame_has_transient) {
            ++block_events;
            last_event_frame = frame_cursor_ + frame;
        }
    }

    frame_cursor_ += frame_count;
    analyzed_frames_.store(frame_cursor_, std::memory_order_relaxed);
    recent_peak_step_bits_.store(FloatBits(block_peak_step), std::memory_order_relaxed);
    StoreMaximum(maximum_sample_step_bits_, block_peak_step);
    if (block_events != 0U) {
        transient_events_.fetch_add(block_events, std::memory_order_relaxed);
        last_event_frame_.store(last_event_frame, std::memory_order_relaxed);
    }
}

void ClickPopDetector::Reset() noexcept {
    previous_samples_.fill(0.0F);
    have_previous_sample_.fill(false);
    frame_cursor_ = 0U;
    analyzed_frames_.store(0U, std::memory_order_relaxed);
    transient_events_.store(0U, std::memory_order_relaxed);
    last_event_frame_.store(0U, std::memory_order_relaxed);
    maximum_sample_step_bits_.store(0U, std::memory_order_relaxed);
    recent_peak_step_bits_.store(0U, std::memory_order_relaxed);
}

ClickPopDiagnostics ClickPopDetector::Diagnostics() const noexcept {
    return ClickPopDiagnostics{
        .analyzed_frames = analyzed_frames_.load(std::memory_order_relaxed),
        .transient_events = transient_events_.load(std::memory_order_relaxed),
        .last_event_frame = last_event_frame_.load(std::memory_order_relaxed),
        .maximum_sample_step =
            BitsFloat(maximum_sample_step_bits_.load(std::memory_order_relaxed)),
        .recent_peak_step =
            BitsFloat(recent_peak_step_bits_.load(std::memory_order_relaxed)),
    };
}

std::uint32_t ClickPopDetector::FloatBits(const float value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
}

float ClickPopDetector::BitsFloat(const std::uint32_t value) noexcept {
    return std::bit_cast<float>(value);
}

void ClickPopDetector::StoreMaximum(std::atomic<std::uint32_t>& destination,
                                    const float value) noexcept {
    const auto value_bits = FloatBits(value);
    auto current_bits = destination.load(std::memory_order_relaxed);
    while (BitsFloat(current_bits) < value &&
           !destination.compare_exchange_weak(current_bits, value_bits,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
    }
}

}  // namespace airplaywin::audio
