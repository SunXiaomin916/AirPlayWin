#include "app/TestTonePlayer.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#include "platform/windows/timing/QpcClock.h"

namespace airplaywin::app {

using airplaywin::audio::AudioFormat;
using airplaywin::audio::AudioOutputMode;
using airplaywin::audio::TestSignalGenerator;
using airplaywin::windows::audio::WasapiOutputOptions;
using airplaywin::windows::audio::WindowsAudioEngine;
using airplaywin::windows::timing::QpcClock;

TestToneResult PlayTestTone(const TestToneRequest& request) {
    TestToneResult result;
    const AudioFormat format{.sample_rate = 48'000U, .channel_count = 2U};
    WindowsAudioEngine engine{WasapiOutputOptions{
        .device_id = request.output_device_id,
        .follow_default_device = request.follow_default_device,
        .output_mode = request.exclusive_audio ? AudioOutputMode::Exclusive
                                               : AudioOutputMode::Shared,
        .low_latency = request.low_latency,
        .allow_shared_fallback = !request.strict_exclusive,
        .ring_capacity_milliseconds = 500U,
        .target_queue_milliseconds =
            request.exclusive_audio ? 10U : (request.low_latency ? 5U : 15U),
    }};
    if (!engine.Open(format)) {
        result.last_error = engine.Diagnostics().last_output_error;
        result.detail = L"Unable to open the selected output device.";
        return result;
    }
    const auto block_frames = 240U;
    std::vector<float> samples(static_cast<std::size_t>(block_frames) *
                               format.channel_count);
    TestSignalGenerator generator{format, request.signal, 0.2F};
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(request.duration_seconds);
    auto submit = [&]() -> bool {
        generator.Fill(samples, block_frames);
        const auto submit_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{1};
        while (std::chrono::steady_clock::now() < submit_deadline) {
            if (engine.Submit(samples, block_frames, QpcClock::Now())) {
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    };
    for (std::uint32_t block = 0U; block < 2U; ++block) {
        if (!submit()) {
            result.last_error = engine.Diagnostics().last_output_error;
            result.detail = L"Unable to prefill the output queue.";
            engine.Close();
            return result;
        }
    }
    if (!engine.Start()) {
        result.last_error = engine.Diagnostics().last_output_error;
        result.detail = L"Unable to start WASAPI rendering.";
        engine.Close();
        return result;
    }
    bool stalled = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!submit()) {
            stalled = true;
            break;
        }
    }
    engine.Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    const auto diagnostics = engine.Diagnostics();
    result.rendered_frames = diagnostics.rendered_frames;
    result.underrun_count = diagnostics.underrun_count;
    engine.Close();
    result.succeeded = !stalled;
    if (!result.succeeded) {
        result.detail = L"The output queue stalled during playback.";
    } else {
        result.detail = L"Test tone finished.";
    }
    return result;
}

}  // namespace airplaywin::app
