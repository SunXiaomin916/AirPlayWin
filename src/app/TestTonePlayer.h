#pragma once

#include <cstdint>
#include <string>

#include "core/audio/TestSignalGenerator.h"
#include "platform/windows/audio/WindowsAudioEngine.h"

namespace airplaywin::app {

// Plays a short diagnostic tone through the selected output without an
// AirPlay session, so the user can verify the audio path from the GUI.
struct TestToneRequest final {
    audio::TestSignal signal{audio::TestSignal::Sine440Hz};
    std::wstring output_device_id{};
    bool follow_default_device{true};
    bool low_latency{false};
    bool exclusive_audio{false};
    bool strict_exclusive{false};
    std::uint32_t duration_seconds{5U};
};

struct TestToneResult final {
    bool succeeded{false};
    std::uint32_t last_error{0U};
    std::uint64_t rendered_frames{0U};
    std::uint64_t underrun_count{0U};
    std::wstring detail{};
};

// Blocking probe. Callers run it on a worker thread; the audio path it
// exercises is identical to the receiver's WASAPI output.
[[nodiscard]] TestToneResult PlayTestTone(const TestToneRequest& request);

}  // namespace airplaywin::app
