#include "TestFramework.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "FakeAudioFrameSink.h"
#include "RtpTestUtils.h"
#include "core/crypto/OpenSessionAuthenticator.h"
#include "core/protocol/AirPlayControlService.h"
#include "platform/windows/network/WindowsRtpTransportController.h"

namespace {

[[nodiscard]] std::vector<std::byte> Bytes(const std::string_view text) {
    const auto* const begin = reinterpret_cast<const std::byte*>(text.data());
    return {begin, begin + text.size()};
}

[[nodiscard]] std::string Text(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] std::string Request(const std::string_view method,
                                  const std::uint32_t cseq,
                                  const std::string_view headers = {},
                                  const std::string_view body = {}) {
    std::string request{method};
    request += " rtsp://127.0.0.1/stream RTSP/1.0\r\nCSeq: ";
    request += std::to_string(cseq);
    request += "\r\n";
    request += headers;
    request += "Content-Length: ";
    request += std::to_string(body.size());
    request += "\r\n\r\n";
    request += body;
    return request;
}

[[nodiscard]] bool ReplyIsOk(const airplaywin::transport::ControlReply& reply) {
    return reply.writes.size() == 1U &&
           Text(reply.writes.front()).find("RTSP/1.0 200 OK") != std::string::npos;
}

void SendPacket(const SOCKET sender,
                const std::uint16_t destination_port,
                const std::span<const std::byte> packet) {
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(destination_port);
    APW_EXPECT(InetPtonA(AF_INET, "127.0.0.1", &destination.sin_addr) == 1);
    const int sent = sendto(sender, reinterpret_cast<const char*>(packet.data()),
                            static_cast<int>(packet.size()), 0,
                            reinterpret_cast<const sockaddr*>(&destination),
                            sizeof(destination));
    APW_EXPECT(sent == static_cast<int>(packet.size()));
}

}  // namespace

void TestRtpTransportIntegration() {
    using airplaywin::protocol::AirPlayControlService;
    using airplaywin::tests::BuildRtpPacket;
    using airplaywin::tests::FakeAudioFrameSink;
    using airplaywin::transport::ConnectionId;
    using airplaywin::windows::network::WindowsRtpTransportController;
    using airplaywin::windows::network::WindowsRtpTransportOptions;

    FakeAudioFrameSink sink;
    WindowsRtpTransportController controller{
        sink, WindowsRtpTransportOptions{.bind_address = "127.0.0.1",
                                         .jitter_capacity_packets = 8U,
                                         .jitter_target_packets = 2U}};
    airplaywin::crypto::OpenSessionAuthenticator authenticator;
    AirPlayControlService service{authenticator,
                                  airplaywin::session::ActiveSessionPolicy::RejectNew,
                                  airplaywin::protocol::ParserLimits{}, &controller};
    constexpr ConnectionId kConnection = 1'001U;
    service.OnConnected(kConnection, "127.0.0.1");

    constexpr std::string_view sdp =
        "v=0\r\n"
        "m=audio 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 L16/44100/2\r\n"
        "a=framesize:96 1\r\n";
    const auto announce = service.OnBytes(
        kConnection,
        Bytes(Request("ANNOUNCE", 1U, "Content-Type: application/sdp\r\n", sdp)));
    APW_EXPECT(ReplyIsOk(announce));

    const auto setup = service.OnBytes(
        kConnection,
        Bytes(Request("SETUP", 2U,
                      "Transport: RTP/AVP/UDP;unicast;mode=record;control_port=6001;"
                      "timing_port=6002\r\n")));
    APW_EXPECT(ReplyIsOk(setup));
    APW_EXPECT(Text(setup.writes.front()).find("server_port=") != std::string::npos);
    const auto after_setup = controller.Diagnostics();
    APW_EXPECT(after_setup.configured);
    APW_EXPECT(after_setup.server_audio_port != 0U);
    APW_EXPECT(after_setup.server_control_port != 0U);
    APW_EXPECT(after_setup.server_timing_port != 0U);

    APW_EXPECT(ReplyIsOk(service.OnBytes(kConnection, Bytes(Request("RECORD", 3U)))));
    APW_EXPECT(controller.Diagnostics().recording);

    const SOCKET sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    APW_EXPECT(sender != INVALID_SOCKET);
    const std::array samples{
        std::byte{0x20U}, std::byte{0x00U}, std::byte{0xE0U}, std::byte{0x00U}};
    const auto packet1 = BuildRtpPacket(1U, 10'000U, samples);
    const auto packet3 = BuildRtpPacket(3U, 10'002U, samples);
    const auto packet4 = BuildRtpPacket(4U, 10'003U, samples);
    SendPacket(sender, after_setup.server_audio_port, packet1);
    SendPacket(sender, after_setup.server_audio_port, packet3);
    SendPacket(sender, after_setup.server_audio_port, packet4);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (sink.SubmittedFrames() < 4U && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    static_cast<void>(closesocket(sender));
    APW_EXPECT(sink.SubmittedFrames() == 4U);
    const auto transport = service.Diagnostics().transport;
    APW_EXPECT(transport.datagrams_received == 3U);
    APW_EXPECT(transport.decoded_packets == 3U);
    APW_EXPECT(transport.concealed_packets == 1U);
    APW_EXPECT(transport.jitter_buffer.lost_packets == 1U);

    const auto volume = service.OnBytes(
        kConnection,
        Bytes(Request("SET_PARAMETER", 4U, "Content-Type: text/parameters\r\n",
                      "volume: -6.0")));
    APW_EXPECT(ReplyIsOk(volume));
    APW_EXPECT_NEAR(sink.Volume(), 0.501187, 0.0001);
    APW_EXPECT(ReplyIsOk(service.OnBytes(kConnection, Bytes(Request("FLUSH", 5U)))));
    APW_EXPECT(sink.FlushCount() == 1U);
    APW_EXPECT(ReplyIsOk(service.OnBytes(kConnection, Bytes(Request("PAUSE", 6U)))));
    APW_EXPECT(ReplyIsOk(service.OnBytes(kConnection, Bytes(Request("RECORD", 7U)))));

    const auto teardown = service.OnBytes(kConnection, Bytes(Request("TEARDOWN", 8U)));
    APW_EXPECT(ReplyIsOk(teardown));
    APW_EXPECT(teardown.close_after_writes);
    APW_EXPECT(!controller.Diagnostics().configured);
    APW_EXPECT(sink.StopCount() == 1U);
    service.OnDisconnected(kConnection,
                           airplaywin::transport::DisconnectReason::Requested);
}
