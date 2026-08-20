#include "TestFramework.h"

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "app/WaveFileReader.h"

namespace {

class TemporaryFile final {
public:
    TemporaryFile() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("airplaywin-wave-reader-" + std::to_string(suffix) + ".wav");
    }

    ~TemporaryFile() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

private:
    std::filesystem::path path_{};
};

void AppendU16(std::vector<char>& bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<char>(value & 0xFFU));
    bytes.push_back(static_cast<char>((value >> 8U) & 0xFFU));
}

void AppendU32(std::vector<char>& bytes, const std::uint32_t value) {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<char>((value >> shift) & 0xFFU));
    }
}

void AppendId(std::vector<char>& bytes, const char (&id)[5]) {
    bytes.insert(bytes.end(), id, id + 4U);
}

void WritePcm16Wave(const std::filesystem::path& path) {
    constexpr std::uint32_t sample_rate = 48'000U;
    constexpr std::uint16_t channels = 2U;
    constexpr std::uint16_t block_align = channels * 2U;
    constexpr std::uint32_t data_size = 8U;
    std::vector<char> bytes;
    bytes.reserve(44U + data_size);
    AppendId(bytes, "RIFF");
    AppendU32(bytes, 36U + data_size);
    AppendId(bytes, "WAVE");
    AppendId(bytes, "fmt ");
    AppendU32(bytes, 16U);
    AppendU16(bytes, 1U);
    AppendU16(bytes, channels);
    AppendU32(bytes, sample_rate);
    AppendU32(bytes, sample_rate * block_align);
    AppendU16(bytes, block_align);
    AppendU16(bytes, 16U);
    AppendId(bytes, "data");
    AppendU32(bytes, data_size);
    AppendU16(bytes, 0U);
    AppendU16(bytes, 16'384U);
    AppendU16(bytes, static_cast<std::uint16_t>(-16'384));
    AppendU16(bytes, 32'767U);

    std::ofstream stream{path, std::ios::binary};
    APW_EXPECT(stream.good());
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    APW_EXPECT(stream.good());
}

void WriteExtensibleFloatWave(const std::filesystem::path& path,
                              const bool valid_subtype = true) {
    constexpr std::uint32_t sample_rate = 48'000U;
    constexpr std::uint16_t channels = 2U;
    constexpr std::uint16_t block_align = channels * 4U;
    constexpr std::uint32_t data_size = 16U;
    std::vector<char> bytes;
    bytes.reserve(68U + data_size);
    AppendId(bytes, "RIFF");
    AppendU32(bytes, 60U + data_size);
    AppendId(bytes, "WAVE");
    AppendId(bytes, "fmt ");
    AppendU32(bytes, 40U);
    AppendU16(bytes, 0xFFFEU);
    AppendU16(bytes, channels);
    AppendU32(bytes, sample_rate);
    AppendU32(bytes, sample_rate * block_align);
    AppendU16(bytes, block_align);
    AppendU16(bytes, 32U);
    AppendU16(bytes, 22U);
    AppendU16(bytes, 32U);
    AppendU32(bytes, 3U);
    AppendU32(bytes, 3U);
    AppendU16(bytes, 0U);
    AppendU16(bytes, 0x0010U);
    const std::array<char, 8U> guid_tail{
        static_cast<char>(0x80), static_cast<char>(0x00), static_cast<char>(0x00),
        static_cast<char>(0xAA), static_cast<char>(0x00), static_cast<char>(0x38),
        static_cast<char>(0x9B), static_cast<char>(valid_subtype ? 0x71 : 0x70)};
    bytes.insert(bytes.end(), guid_tail.begin(), guid_tail.end());
    AppendId(bytes, "data");
    AppendU32(bytes, data_size);
    AppendU32(bytes, std::bit_cast<std::uint32_t>(0.25F));
    AppendU32(bytes, std::bit_cast<std::uint32_t>(-0.25F));
    AppendU32(bytes, std::bit_cast<std::uint32_t>(1.0F));
    AppendU32(bytes, std::bit_cast<std::uint32_t>(-1.0F));

    std::ofstream stream{path, std::ios::binary};
    APW_EXPECT(stream.good());
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    APW_EXPECT(stream.good());
}

void WriteTruncatedWave(const std::filesystem::path& path) {
    std::vector<char> bytes;
    AppendId(bytes, "RIFF");
    AppendU32(bytes, 100U);
    AppendId(bytes, "WAVE");
    AppendId(bytes, "fmt ");
    AppendU32(bytes, 100U);
    std::ofstream stream{path, std::ios::binary};
    APW_EXPECT(stream.good());
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    APW_EXPECT(stream.good());
}

}  // namespace

void TestWaveFileReader() {
    TemporaryFile file;
    WritePcm16Wave(file.Path());

    airplaywin::app::WaveCapture capture;
    std::wstring error;
    APW_EXPECT(airplaywin::app::ReadWaveCapture(file.Path(), capture, error));
    APW_EXPECT(error.empty());
    APW_EXPECT(capture.sample_rate == 48'000U);
    APW_EXPECT(capture.channel_count == 2U);
    APW_EXPECT(capture.interleaved_samples.size() == 4U);
    APW_EXPECT(capture.interleaved_samples[0] == 0.0F);
    APW_EXPECT(capture.interleaved_samples[1] == 0.5F);
    APW_EXPECT(capture.interleaved_samples[2] == -0.5F);
    APW_EXPECT(capture.interleaved_samples[3] > 0.999F);

    WriteExtensibleFloatWave(file.Path());
    APW_EXPECT(airplaywin::app::ReadWaveCapture(file.Path(), capture, error));
    APW_EXPECT(capture.sample_rate == 48'000U);
    APW_EXPECT(capture.channel_count == 2U);
    APW_EXPECT(capture.interleaved_samples.size() == 4U);
    APW_EXPECT(capture.interleaved_samples[0] == 0.25F);
    APW_EXPECT(capture.interleaved_samples[1] == -0.25F);
    APW_EXPECT(capture.interleaved_samples[2] == 1.0F);
    APW_EXPECT(capture.interleaved_samples[3] == -1.0F);

    WriteExtensibleFloatWave(file.Path(), false);
    APW_EXPECT(!airplaywin::app::ReadWaveCapture(file.Path(), capture, error));
    APW_EXPECT(!error.empty());

    WriteTruncatedWave(file.Path());
    APW_EXPECT(!airplaywin::app::ReadWaveCapture(file.Path(), capture, error));
    APW_EXPECT(!error.empty());

    APW_EXPECT(!airplaywin::app::ReadWaveCapture(file.Path().wstring() + L".missing",
                                                 capture, error));
    APW_EXPECT(!error.empty());
}
