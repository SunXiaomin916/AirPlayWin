#include "platform/windows/audio/WindowsAudioEngine.h"

#include <memory>
#include <utility>

namespace airplaywin::windows::audio {

WindowsAudioEngine::WindowsAudioEngine(WasapiOutputOptions options)
    : engine_(std::make_unique<WasapiAudioOutput>(std::move(options))) {}

WindowsAudioEngine::WindowsAudioEngine(
    std::unique_ptr<airplaywin::audio::IAudioOutput> output)
    : engine_(std::move(output)) {}

bool WindowsAudioEngine::Open(const airplaywin::audio::AudioFormat& format) {
    return engine_.Open(format);
}

bool WindowsAudioEngine::Start() {
    return engine_.Start();
}

bool WindowsAudioEngine::Submit(const std::span<const float> interleaved_samples,
                                const std::uint32_t frame_count,
                                const std::int64_t timestamp_qpc) noexcept {
    return engine_.Submit(interleaved_samples, frame_count, timestamp_qpc);
}

bool WindowsAudioEngine::Submit(const airplaywin::audio::AudioBuffer& buffer) noexcept {
    return engine_.Submit(buffer);
}

void WindowsAudioEngine::Pause() noexcept {
    engine_.Pause();
}

void WindowsAudioEngine::Resume() noexcept {
    engine_.Resume();
}

void WindowsAudioEngine::Stop() noexcept {
    engine_.Stop();
}

void WindowsAudioEngine::Flush() noexcept {
    engine_.Flush();
}

void WindowsAudioEngine::Seek() noexcept {
    engine_.Seek();
}

void WindowsAudioEngine::HardResync() noexcept {
    engine_.HardResync();
}

void WindowsAudioEngine::ReplaceSender() noexcept {
    engine_.ReplaceSender();
}

void WindowsAudioEngine::ResetSession() noexcept {
    engine_.ResetSession();
}

bool WindowsAudioEngine::ChangeFormat(const airplaywin::audio::AudioFormat& format) {
    return engine_.ChangeFormat(format);
}

void WindowsAudioEngine::SetVolume(const float linear_gain) noexcept {
    engine_.SetVolume(linear_gain);
}

void WindowsAudioEngine::Close() noexcept {
    engine_.Close();
}

std::uint64_t WindowsAudioEngine::CurrentEpoch() const noexcept {
    return engine_.CurrentEpoch();
}

airplaywin::audio::AudioDiagnosticsSnapshot WindowsAudioEngine::Diagnostics() const {
    return engine_.Diagnostics();
}

}  // namespace airplaywin::windows::audio
