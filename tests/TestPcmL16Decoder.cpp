#include "TestFramework.h"

#include <array>
#include <cstddef>

#include "core/audio/PcmL16Decoder.h"

void TestPcmL16Decoder() {
    using airplaywin::audio::DecodeStatus;
    using airplaywin::audio::EncodedAudioFormat;
    using airplaywin::audio::EncodedAudioFrame;
    using airplaywin::audio::PcmL16Decoder;

    PcmL16Decoder decoder;
    const std::array<std::byte, 4U> tiny_payload{};
    std::array<float, 8U> output{};
    APW_EXPECT(decoder.Decode(EncodedAudioFrame{.payload = tiny_payload}, output).status ==
               DecodeStatus::NotConfigured);
    APW_EXPECT(decoder.Configure(EncodedAudioFormat{.sample_rate = 44'100U,
                                                    .channel_count = 2U,
                                                    .payload_type = 96U,
                                                    .nominal_frames_per_packet = 2U}));

    const std::array payload{
        std::byte{0x80U}, std::byte{0x00U},
        std::byte{0x00U}, std::byte{0x00U},
        std::byte{0x7FU}, std::byte{0xFFU},
        std::byte{0xFFU}, std::byte{0xFFU},
    };
    const auto result = decoder.Decode(EncodedAudioFrame{.payload = payload}, output);
    APW_EXPECT(result.status == DecodeStatus::Ok);
    APW_EXPECT(result.frame_count == 2U);
    APW_EXPECT_NEAR(output[0], -1.0, 0.00001);
    APW_EXPECT_NEAR(output[1], 0.0, 0.00001);
    APW_EXPECT_NEAR(output[2], 32767.0 / 32768.0, 0.00001);
    APW_EXPECT_NEAR(output[3], -1.0 / 32768.0, 0.00001);

    const std::array malformed{std::byte{0}, std::byte{1}, std::byte{2}};
    APW_EXPECT(decoder.Decode(EncodedAudioFrame{.payload = malformed}, output).status ==
               DecodeStatus::MalformedPayload);
    std::array<float, 1U> too_small{};
    APW_EXPECT(decoder.Decode(EncodedAudioFrame{.payload = payload}, too_small).status ==
               DecodeStatus::OutputTooSmall);
    output.fill(1.0F);
    const auto concealed = decoder.ConcealLoss(2U, output);
    APW_EXPECT(concealed.status == DecodeStatus::Ok);
    APW_EXPECT(concealed.frame_count == 2U);
    APW_EXPECT(output[0] == 0.0F && output[3] == 0.0F);
}
