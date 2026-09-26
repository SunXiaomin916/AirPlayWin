#include "TestFramework.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <set>
#include <string>
#include <thread>

#include "core/protocol/SdpAudioParser.h"
#include "platform/windows/audio/WindowsAlacDecoder.h"
#include "platform/windows/audio/WindowsAudioDeviceEnumerator.h"
#include "platform/windows/crypto/WindowsRaopCryptoProvider.h"
#include "platform/windows/timing/QpcClock.h"

void TestWindowsPlatform() {
    using airplaywin::windows::audio::WindowsAudioDeviceEnumerator;
    using airplaywin::windows::audio::WindowsAlacDecoder;
    using airplaywin::windows::crypto::WindowsRaopCryptoProvider;
    using airplaywin::windows::timing::QpcClock;

    APW_EXPECT(QpcClock::Frequency() > 0);
    const auto first_tick = QpcClock::Now();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto second_tick = QpcClock::Now();
    APW_EXPECT(second_tick > first_tick);
    APW_EXPECT(QpcClock::ToSeconds(second_tick - first_tick) > 0.0);

    const auto devices = WindowsAudioDeviceEnumerator::EnumerateRenderDevices();
    std::set<std::wstring> device_ids;
    std::size_t default_count = 0U;
    for (const auto& device : devices) {
        APW_EXPECT(!device.id.empty());
        APW_EXPECT(!device.friendly_name.empty());
        APW_EXPECT(device_ids.insert(device.id).second);
        default_count += device.is_default ? 1U : 0U;
    }
    APW_EXPECT(default_count <= 1U);

    WindowsRaopCryptoProvider raop_crypto{
        airplaywin::discovery::DeviceId{0x02U, 0x11U, 0x22U, 0xAAU, 0xBBU, 0xCCU}};
    APW_EXPECT(raop_crypto.Available());
    const auto apple_response = raop_crypto.BuildAppleResponse(
        "cLi758x3X3KV46A7Spu+rQ", "192.0.2.10");
    APW_EXPECT(apple_response.has_value());
    APW_EXPECT(apple_response->size() == 342U);
    APW_EXPECT(apple_response->find_first_of("=\r\n") == std::string::npos);
    APW_EXPECT(raop_crypto.BuildAppleResponse(
                   "cLi758x3X3KV46A7Spu+rQ", "192.0.2.10") == apple_response);
    APW_EXPECT(!raop_crypto.BuildAppleResponse("invalid", "192.0.2.10").has_value());
    APW_EXPECT(!raop_crypto.BuildAppleResponse(
                    "cLi758x3X3KV46A7Spu+rQ", "not-an-address").has_value());
    APW_EXPECT(!raop_crypto.DecryptAesMaterial("invalid", "invalid").has_value());
    const auto aes_material = raop_crypto.DecryptAesMaterial(
        "pzqwEr7OhQ00d7nxq+tgbhzkDeEov+OeQe/h3Tr1y1E9f/l6QLaPAoflu+ofoWAJ"
        "Ua4tJGiasVPO7wQde7irEHFCrX/jdldEa+2Q1iRs7AE1Zgq/QyHgysEmRAqTn8Wc"
        "5NaaThbntInM5YhOEs5rjBNFo9+3ho4uo+YQs6IGNtbBsmhwK0JHYAwUq1iXyZoi"
        "Nf7mnDJV1nuvAyN16s/vneCqNamorwcnM1c1BG21Uh2HRUGktvIleNNF4dbI/Z5S"
        "yIWW2zls77ly1sKmtFqiHfceWvTFUwgnVhSjau3RMcGDbOxwtS/2vsAIg9vES/TO"
        "TROi4pUotQR5zg/XuYoOxg",
        "EBESExQVFhcYGRobHB0eHw");
    APW_EXPECT(aes_material.has_value());
    for (std::size_t index = 0U; index < 16U; ++index) {
        APW_EXPECT(aes_material->key[index] == static_cast<std::byte>(index));
        APW_EXPECT(aes_material->iv[index] == static_cast<std::byte>(index + 16U));
    }

    const auto alac_description = airplaywin::protocol::ParseSdpAudioSession(
        "m=audio 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 AppleLossless\r\n"
        "a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n");
    APW_EXPECT(alac_description.has_value());
    WindowsAlacDecoder alac_decoder;
    APW_EXPECT(alac_decoder.Configure(alac_description->format));
    std::array<float, 704U> concealed{};
    const auto conceal_result = alac_decoder.ConcealLoss(352U, concealed);
    APW_EXPECT(conceal_result.status == airplaywin::audio::DecodeStatus::Ok);
    APW_EXPECT(conceal_result.frame_count == 352U);
}
