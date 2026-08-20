#include "TestFramework.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/crypto/OpenSessionAuthenticator.h"
#include "core/protocol/AirPlayControlService.h"

namespace {

[[nodiscard]] std::vector<std::byte> Bytes(const std::string_view text) {
    const auto* const begin = reinterpret_cast<const std::byte*>(text.data());
    return {begin, begin + text.size()};
}

[[nodiscard]] std::vector<std::byte> ReadFixture(const std::string_view name) {
    const std::string_view fixture_root{AIRPLAYWIN_TEST_FIXTURE_DIR};
    std::u8string fixture_root_utf8;
    fixture_root_utf8.reserve(fixture_root.size());
    for (const unsigned char value : fixture_root) {
        fixture_root_utf8.push_back(static_cast<char8_t>(value));
    }
    const std::filesystem::path path =
        std::filesystem::path{fixture_root_utf8} / "rtsp" / name;
    std::ifstream input{path, std::ios::binary};
    APW_EXPECT(input.good());
    const std::string encoded{std::istreambuf_iterator<char>{input},
                              std::istreambuf_iterator<char>{}};
    std::string content;
    content.reserve(encoded.size());
    for (std::size_t index = 0U; index < encoded.size(); ++index) {
        if (encoded[index] == '\\' && index + 1U < encoded.size() &&
            (encoded[index + 1U] == 'r' || encoded[index + 1U] == 'n')) {
            content.push_back(encoded[index + 1U] == 'r' ? '\r' : '\n');
            ++index;
        } else if (encoded[index] != '\r' && encoded[index] != '\n') {
            content.push_back(encoded[index]);
        }
    }
    return Bytes(content);
}

[[nodiscard]] std::string Text(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

}  // namespace

void TestAirPlayControlService() {
    airplaywin::crypto::OpenSessionAuthenticator authenticator;
    airplaywin::protocol::AirPlayControlService service{authenticator};
    constexpr airplaywin::transport::ConnectionId kFirst = 100U;
    constexpr airplaywin::transport::ConnectionId kSecond = 101U;
    service.OnConnected(kFirst, "127.0.0.1");

    const auto options = ReadFixture("options_request.txt");
    const auto partial = service.OnBytes(kFirst, std::span{options}.first(5U));
    APW_EXPECT(partial.writes.empty());
    const auto options_reply = service.OnBytes(kFirst, std::span{options}.subspan(5U));
    APW_EXPECT(options_reply.writes.size() == 1U);
    const auto options_text = Text(options_reply.writes.front());
    APW_EXPECT(options_text.find("RTSP/1.0 200 OK") != std::string::npos);
    APW_EXPECT(options_text.find("CSeq: 1") != std::string::npos);
    APW_EXPECT(options_text.find("Public: ANNOUNCE") != std::string::npos);

    const auto announce = ReadFixture("announce_request.txt");
    const auto announce_reply = service.OnBytes(kFirst, announce);
    APW_EXPECT(announce_reply.writes.size() == 1U);
    APW_EXPECT(Text(announce_reply.writes.front()).find("200 OK") != std::string::npos);
    const auto first_session = service.Sessions().front();
    APW_EXPECT(first_session.owns_playback);
    APW_EXPECT(first_session.state == airplaywin::session::SessionState::Announced);

    service.OnConnected(kSecond, "192.0.2.2");
    const auto rejected = service.OnBytes(kSecond, announce);
    APW_EXPECT(rejected.writes.size() == 1U);
    APW_EXPECT(Text(rejected.writes.front()).find("453 Not Enough Bandwidth") !=
               std::string::npos);

    const auto volume = Bytes(
        "SET_PARAMETER rtsp://127.0.0.1/stream RTSP/1.0\r\nCSeq: 3\r\n"
        "Content-Type: text/parameters\r\nContent-Length: 13\r\n\r\nvolume: -18.0");
    const auto volume_reply = service.OnBytes(kFirst, volume);
    APW_EXPECT(Text(volume_reply.writes.front()).find("200 OK") != std::string::npos);

    const auto setup = Bytes(
        "SETUP rtsp://127.0.0.1/stream RTSP/1.0\r\nCSeq: 4\r\n"
        "Transport: RTP/AVP/UDP;unicast\r\nContent-Length: 0\r\n\r\n");
    const auto setup_reply = service.OnBytes(kFirst, setup);
    APW_EXPECT(Text(setup_reply.writes.front()).find("461 Unsupported Transport") !=
               std::string::npos);

    const auto pairing = Bytes(
        "POST /pair-setup HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
    const auto pairing_reply = service.OnBytes(kFirst, pairing);
    APW_EXPECT(Text(pairing_reply.writes.front()).find("501 Not Implemented") !=
               std::string::npos);

    constexpr airplaywin::transport::ConnectionId kInvalid = 102U;
    service.OnConnected(kInvalid, "192.0.2.3");
    const auto invalid_cseq = Bytes(
        "OPTIONS * RTSP/1.0\r\nCSeq: not-a-number\r\nContent-Length: 0\r\n\r\n");
    const auto invalid_reply = service.OnBytes(kInvalid, invalid_cseq);
    APW_EXPECT(invalid_reply.close_after_writes);
    APW_EXPECT(Text(invalid_reply.writes.front()).find("400 Bad Request") !=
               std::string::npos);

    const auto teardown = Bytes(
        "TEARDOWN rtsp://127.0.0.1/stream RTSP/1.0\r\nCSeq: 5\r\n"
        "Content-Length: 0\r\n\r\n");
    const auto teardown_reply = service.OnBytes(kFirst, teardown);
    APW_EXPECT(teardown_reply.close_after_writes);
    APW_EXPECT(Text(teardown_reply.writes.front()).find("Connection: close") !=
               std::string::npos);

    service.OnDisconnected(kFirst, airplaywin::transport::DisconnectReason::Requested);
    service.OnDisconnected(kSecond, airplaywin::transport::DisconnectReason::ServerShutdown);
    service.OnDisconnected(kInvalid, airplaywin::transport::DisconnectReason::ProtocolError);
    const auto diagnostics = service.Diagnostics();
    APW_EXPECT(diagnostics.received_requests == 8U);
    APW_EXPECT(diagnostics.sessions.active_connections == 0U);
    APW_EXPECT(diagnostics.sessions.rejected_sessions == 1U);
    APW_EXPECT(diagnostics.pairing_requests == 1U);
    APW_EXPECT(diagnostics.unsupported_requests == 2U);
}
