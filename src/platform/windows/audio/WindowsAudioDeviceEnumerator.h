#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace airplaywin::windows::audio {

struct AudioDeviceInfo final {
    std::wstring id;
    std::wstring friendly_name;
    std::uint32_t state{0U};
    bool is_default{false};
};

class WindowsAudioDeviceEnumerator final {
public:
    [[nodiscard]] static std::vector<AudioDeviceInfo> EnumerateRenderDevices();
    [[nodiscard]] static std::wstring DefaultRenderDeviceId();
};

}  // namespace airplaywin::windows::audio
