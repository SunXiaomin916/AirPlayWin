#include "app/WaveFileReader.h"

#include <bit>
#include <cstddef>
#include <fstream>
#include <limits>
#include <span>

namespace airplaywin::app {
namespace {

[[nodiscard]] std::uint16_t ReadU16(const std::span<const std::byte> bytes,
                                    const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
           static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U]))
               << 8U);
}

[[nodiscard]] std::uint32_t ReadU32(const std::span<const std::byte> bytes,
                                    const std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U]))
            << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2U]))
            << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 3U]))
            << 24U);
}

[[nodiscard]] bool Matches(const std::span<const std::byte> bytes,
                           const std::size_t offset,
                           const char (&text)[5]) noexcept {
    return offset + 4U <= bytes.size() &&
           std::to_integer<char>(bytes[offset]) == text[0] &&
           std::to_integer<char>(bytes[offset + 1U]) == text[1] &&
           std::to_integer<char>(bytes[offset + 2U]) == text[2] &&
           std::to_integer<char>(bytes[offset + 3U]) == text[3];
}

}  // namespace

bool ReadWaveCapture(const std::filesystem::path& path,
                     WaveCapture& capture,
                     std::wstring& error) {
    capture = {};
    error.clear();
    std::ifstream stream{path, std::ios::binary | std::ios::ate};
    if (!stream) {
        error = L"unable to open capture file";
        return false;
    }
    const auto end = stream.tellg();
    if (end <= 0 || static_cast<std::uintmax_t>(end) >
                        static_cast<std::uintmax_t>(
                            std::numeric_limits<std::streamsize>::max())) {
        error = L"capture file size is invalid";
        return false;
    }
    const auto size = static_cast<std::size_t>(end);
    std::vector<std::byte> storage(size);
    stream.seekg(0, std::ios::beg);
    if (!stream.read(reinterpret_cast<char*>(storage.data()),
                     static_cast<std::streamsize>(storage.size()))) {
        error = L"capture file could not be read";
        return false;
    }
    const auto bytes = std::span<const std::byte>{storage};
    if (bytes.size() < 12U || !Matches(bytes, 0U, "RIFF") ||
        !Matches(bytes, 8U, "WAVE")) {
        error = L"capture is not a RIFF/WAVE file";
        return false;
    }

    std::size_t format_offset = 0U;
    std::size_t format_size = 0U;
    std::size_t data_offset = 0U;
    std::size_t data_size = 0U;
    for (std::size_t offset = 12U; offset + 8U <= bytes.size();) {
        const auto chunk_size = static_cast<std::size_t>(ReadU32(bytes, offset + 4U));
        const auto payload_offset = offset + 8U;
        if (chunk_size > bytes.size() - payload_offset) {
            error = L"WAVE chunk exceeds file bounds";
            return false;
        }
        if (Matches(bytes, offset, "fmt ")) {
            format_offset = payload_offset;
            format_size = chunk_size;
        } else if (Matches(bytes, offset, "data")) {
            data_offset = payload_offset;
            data_size = chunk_size;
        }
        const auto padded_size = chunk_size + (chunk_size & 1U);
        if (padded_size > bytes.size() - payload_offset) {
            break;
        }
        offset = payload_offset + padded_size;
    }
    if (format_size < 16U || data_size == 0U) {
        error = L"WAVE fmt or data chunk is missing";
        return false;
    }

    auto format_tag = ReadU16(bytes, format_offset);
    const auto channel_count = ReadU16(bytes, format_offset + 2U);
    const auto sample_rate = ReadU32(bytes, format_offset + 4U);
    const auto block_align = ReadU16(bytes, format_offset + 12U);
    const auto bits_per_sample = ReadU16(bytes, format_offset + 14U);
    constexpr std::uint16_t kWaveFormatExtensible = 0xFFFEU;
    if (format_tag == kWaveFormatExtensible) {
        if (format_size < 40U) {
            error = L"WAVE extensible format is truncated";
            return false;
        }
        const auto subformat = ReadU32(bytes, format_offset + 24U);
        format_tag = static_cast<std::uint16_t>(subformat);
    }
    const bool pcm16 = format_tag == 1U && bits_per_sample == 16U;
    const bool float32 = format_tag == 3U && bits_per_sample == 32U;
    const auto bytes_per_sample = static_cast<std::uint16_t>(bits_per_sample / 8U);
    if ((!pcm16 && !float32) || channel_count == 0U || channel_count > 8U ||
        sample_rate < 8'000U || sample_rate > 384'000U ||
        block_align != channel_count * bytes_per_sample ||
        data_size % block_align != 0U) {
        error = L"WAVE format must be PCM16 or float32 with 1-8 channels";
        return false;
    }

    const auto sample_count = data_size / bytes_per_sample;
    try {
        capture.interleaved_samples.resize(sample_count);
    } catch (...) {
        error = L"capture sample allocation failed";
        return false;
    }
    capture.sample_rate = sample_rate;
    capture.channel_count = channel_count;
    for (std::size_t index = 0U; index < sample_count; ++index) {
        const auto sample_offset = data_offset + index * bytes_per_sample;
        if (pcm16) {
            const auto value = static_cast<std::int16_t>(ReadU16(bytes, sample_offset));
            capture.interleaved_samples[index] =
                static_cast<float>(value) / 32'768.0F;
        } else {
            capture.interleaved_samples[index] =
                std::bit_cast<float>(ReadU32(bytes, sample_offset));
        }
    }
    return true;
}

}  // namespace airplaywin::app
