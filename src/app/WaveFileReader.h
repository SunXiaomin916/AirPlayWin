#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace airplaywin::app {

struct WaveCapture final {
    std::uint32_t sample_rate{0U};
    std::uint16_t channel_count{0U};
    std::vector<float> interleaved_samples{};
};

[[nodiscard]] bool ReadWaveCapture(const std::filesystem::path& path,
                                   WaveCapture& capture,
                                   std::wstring& error);

}  // namespace airplaywin::app
