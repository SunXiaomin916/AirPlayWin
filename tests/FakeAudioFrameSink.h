#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "core/audio/IAudioFrameSink.h"

namespace airplaywin::tests {

class FakeAudioFrameSink final : public audio::IAudioFrameSink {
public:
    [[nodiscard]] bool Configure(const audio::AudioFormat& format) override {
        std::scoped_lock lock{mutex_};
        format_ = format;
        configured_ = format.IsValid();
        return configured_;
    }

    [[nodiscard]] bool Start() override {
        std::scoped_lock lock{mutex_};
        started_ = configured_;
        ++start_count_;
        return started_;
    }

    [[nodiscard]] bool Submit(const audio::DecodedAudioFrameView& frame) noexcept override {
        try {
            std::scoped_lock lock{mutex_};
            if (!started_) {
                return false;
            }
            samples_.insert(samples_.end(), frame.interleaved_samples.begin(),
                            frame.interleaved_samples.end());
            frame_counts_.push_back(frame.frame_count);
            timestamps_.push_back(frame.rtp_timestamp);
            concealed_.push_back(frame.concealed);
            submitted_frames_ += frame.frame_count;
            return true;
        } catch (...) {
            return false;
        }
    }

    void Pause() noexcept override {
        std::scoped_lock lock{mutex_};
        started_ = false;
        ++pause_count_;
    }

    void Resume() noexcept override {
        std::scoped_lock lock{mutex_};
        started_ = configured_;
        ++resume_count_;
    }

    void Flush() noexcept override {
        std::scoped_lock lock{mutex_};
        ++flush_count_;
    }

    void SetVolume(const float linear_gain) noexcept override {
        std::scoped_lock lock{mutex_};
        volume_ = linear_gain;
    }

    void Stop() noexcept override {
        std::scoped_lock lock{mutex_};
        started_ = false;
        configured_ = false;
        ++stop_count_;
    }

    [[nodiscard]] std::uint64_t SubmittedFrames() const noexcept {
        std::scoped_lock lock{mutex_};
        return submitted_frames_;
    }

    [[nodiscard]] std::vector<float> Samples() const {
        std::scoped_lock lock{mutex_};
        return samples_;
    }

    [[nodiscard]] std::vector<bool> Concealed() const {
        std::scoped_lock lock{mutex_};
        return concealed_;
    }

    [[nodiscard]] float Volume() const noexcept {
        std::scoped_lock lock{mutex_};
        return volume_;
    }

    [[nodiscard]] std::uint64_t FlushCount() const noexcept {
        std::scoped_lock lock{mutex_};
        return flush_count_;
    }

    [[nodiscard]] std::uint64_t StopCount() const noexcept {
        std::scoped_lock lock{mutex_};
        return stop_count_;
    }

private:
    mutable std::mutex mutex_{};
    audio::AudioFormat format_{};
    bool configured_{false};
    bool started_{false};
    float volume_{1.0F};
    std::uint64_t submitted_frames_{0U};
    std::uint64_t start_count_{0U};
    std::uint64_t pause_count_{0U};
    std::uint64_t resume_count_{0U};
    std::uint64_t flush_count_{0U};
    std::uint64_t stop_count_{0U};
    std::vector<float> samples_{};
    std::vector<std::uint32_t> frame_counts_{};
    std::vector<std::uint32_t> timestamps_{};
    std::vector<bool> concealed_{};
};

}  // namespace airplaywin::tests
