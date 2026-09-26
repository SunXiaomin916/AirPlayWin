#include "TestFramework.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/crypto/OpenSessionAuthenticator.h"
#include "core/crypto/IRaopCryptoProvider.h"
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

class TestRaopCryptoProvider final : public airplaywin::crypto::IRaopCryptoProvider {
public:
    [[nodiscard]] std::optional<std::string> BuildAppleResponse(
        const std::string_view apple_challenge,
        const std::string_view local_address) const override {
        if (apple_challenge == "dGVzdC1jaGFsbGVuZ2U" && local_address == "192.0.2.10") {
            return "signed-test-response";
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<airplaywin::crypto::RaopAesMaterial> DecryptAesMaterial(
        const std::string_view encrypted_key,
        const std::string_view initialization_vector) const override {
        if (encrypted_key != "encrypted-test-key" ||
            initialization_vector != "test-initialization-vector") {
            return std::nullopt;
        }
        airplaywin::crypto::RaopAesMaterial material;
        material.key.fill(std::byte{0x11});
        material.iv.fill(std::byte{0x22});
        return material;
    }
};

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

    const auto progress = Bytes(
        "SET_PARAMETER rtsp://127.0.0.1/stream RTSP/1.0\r\nCSeq: 31\r\n"
        "Content-Type: text/parameters\r\nContent-Length: 20\r\n\r\nprogress: 1/2/3000\r\n");
    const auto progress_reply = service.OnBytes(kFirst, progress);
    APW_EXPECT(Text(progress_reply.writes.front()).find("200 OK") != std::string::npos);

    const auto metadata = Bytes(
        "SET_PARAMETER rtsp://127.0.0.1/stream RTSP/1.0\r\nCSeq: 32\r\n"
        "Content-Type: application/x-dmap-tagged\r\nContent-Length: 4\r\n\r\nmlit");
    const auto metadata_reply = service.OnBytes(kFirst, metadata);
    APW_EXPECT(Text(metadata_reply.writes.front()).find("200 OK") != std::string::npos);

    const auto artwork = Bytes(
        "SET_PARAMETER rtsp://127.0.0.1/stream RTSP/1.0\r\nCSeq: 33\r\n"
        "Content-Type: image/jpeg\r\nContent-Length: 3\r\n\r\nimg");
    const auto artwork_reply = service.OnBytes(kFirst, artwork);
    APW_EXPECT(Text(artwork_reply.writes.front()).find("200 OK") != std::string::npos);

    const auto malformed_volume = Bytes(
        "SET_PARAMETER rtsp://127.0.0.1/stream RTSP/1.0\r\nCSeq: 34\r\n"
        "Content-Type: text/parameters\r\nContent-Length: 12\r\n\r\nvolume: loud");
    const auto malformed_volume_reply = service.OnBytes(kFirst, malformed_volume);
    APW_EXPECT(Text(malformed_volume_reply.writes.front()).find("451 Invalid Parameter") !=
               std::string::npos);

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

    const std::string command_body{"bplist00compatibility-metadata"};
    const auto command = Bytes(
        "POST /command RTSP/1.0\r\nCSeq: 4\r\n"
        "Content-Type: application/x-apple-binary-plist\r\nContent-Length: " +
        std::to_string(command_body.size()) + "\r\n\r\n" + command_body);
    const auto command_reply = service.OnBytes(kFirst, command);
    APW_EXPECT(Text(command_reply.writes.front()).find("200 OK") != std::string::npos);

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
    APW_EXPECT(diagnostics.received_requests == 13U);
    APW_EXPECT(diagnostics.sessions.active_connections == 0U);
    APW_EXPECT(diagnostics.sessions.rejected_sessions == 1U);
    APW_EXPECT(diagnostics.pairing_requests == 1U);
    APW_EXPECT(diagnostics.unsupported_requests == 2U);
    APW_EXPECT(diagnostics.recent_requests.size() == 13U);
    APW_EXPECT(diagnostics.recent_requests.front().method == "OPTIONS");
    APW_EXPECT(diagnostics.recent_requests.front().response_status == 200);
    APW_EXPECT(diagnostics.recent_requests.back().method == "TEARDOWN");
    APW_EXPECT(diagnostics.recent_requests.back().response_status == 200);
    const auto pairing_trace = std::ranges::find_if(
        diagnostics.recent_requests,
        [](const airplaywin::protocol::ControlRequestTrace& trace) {
            return trace.target == "/pair-setup";
        });
    APW_EXPECT(pairing_trace != diagnostics.recent_requests.end());
    APW_EXPECT(pairing_trace->response_status == 501);

    TestRaopCryptoProvider raop_crypto;
    airplaywin::protocol::AirPlayControlService classic_service{
        authenticator, airplaywin::session::ActiveSessionPolicy::RejectNew,
        airplaywin::protocol::ParserLimits{}, nullptr, &raop_crypto};
    constexpr airplaywin::transport::ConnectionId kClassic = 200U;
    classic_service.OnConnected(kClassic, "192.0.2.20", "192.0.2.10");
    const auto classic_options = Bytes(
        "OPTIONS * RTSP/1.0\r\nCSeq: 0\r\n"
        "Apple-Challenge: dGVzdC1jaGFsbGVuZ2U\r\nContent-Length: 0\r\n\r\n");
    const auto classic_reply = classic_service.OnBytes(kClassic, classic_options);
    APW_EXPECT(classic_reply.writes.size() == 1U);
    const auto classic_text = Text(classic_reply.writes.front());
    APW_EXPECT(classic_text.find("RTSP/1.0 200 OK") != std::string::npos);
    APW_EXPECT(classic_text.find("Server: AirTunes/105.1") != std::string::npos);
    APW_EXPECT(classic_text.find("Apple-Response: signed-test-response") !=
               std::string::npos);
    APW_EXPECT(classic_text.find("Audio-Jack-Status: connected; type=analog") !=
               std::string::npos);
    const std::string encrypted_sdp =
        "v=0\r\n"
        "m=audio 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 AppleLossless\r\n"
        "a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n"
        "a=rsaaeskey:encrypted-test-key\r\n"
        "a=aesiv:test-initialization-vector\r\n";
    const auto classic_announce = Bytes(
        "ANNOUNCE rtsp://192.0.2.10/stream RTSP/1.0\r\nCSeq: 1\r\n"
        "Content-Type: application/sdp\r\nContent-Length: " +
        std::to_string(encrypted_sdp.size()) + "\r\n\r\n" + encrypted_sdp);
    const auto classic_announce_reply =
        classic_service.OnBytes(kClassic, classic_announce);
    APW_EXPECT(classic_announce_reply.writes.size() == 1U);
    APW_EXPECT(Text(classic_announce_reply.writes.front()).find("200 OK") !=
               std::string::npos);
    const auto classic_diagnostics = classic_service.Diagnostics();
    APW_EXPECT(classic_diagnostics.recent_requests.size() == 2U);
    APW_EXPECT(classic_diagnostics.recent_requests.front().apple_challenge_present);
    APW_EXPECT(classic_diagnostics.recent_requests.front().apple_response_sent);
}
