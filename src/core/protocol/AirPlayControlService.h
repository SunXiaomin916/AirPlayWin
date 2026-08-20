#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "core/crypto/ISessionAuthenticator.h"
#include "core/protocol/IncrementalRtspParser.h"
#include "core/session/SessionManager.h"
#include "core/transport/IControlConnectionHandler.h"
#include "core/transport/IAudioTransportController.h"

namespace airplaywin::protocol {

struct ControlDiagnostics final {
    session::SessionDiagnostics sessions{};
    std::uint64_t received_requests{0U};
    std::uint64_t parse_errors{0U};
    std::uint64_t rejected_requests{0U};
    std::uint64_t unsupported_requests{0U};
    std::uint64_t pairing_requests{0U};
    transport::AudioTransportDiagnostics transport{};
    std::string last_error{};
};

class AirPlayControlService final : public transport::IControlConnectionHandler {
public:
    explicit AirPlayControlService(
        const crypto::ISessionAuthenticator& authenticator,
        session::ActiveSessionPolicy policy = session::ActiveSessionPolicy::RejectNew,
        ParserLimits parser_limits = {},
        transport::IAudioTransportController* audio_transport = nullptr);

    void OnConnected(transport::ConnectionId connection_id,
                     std::string_view peer_address) override;
    [[nodiscard]] transport::ControlReply OnBytes(
        transport::ConnectionId connection_id,
        std::span<const std::byte> bytes) override;
    void OnDisconnected(transport::ConnectionId connection_id,
                        transport::DisconnectReason reason) override;

    [[nodiscard]] ControlDiagnostics Diagnostics() const;
    [[nodiscard]] std::vector<session::SessionSnapshot> Sessions() const;

private:
    struct ConnectionContext final {
        explicit ConnectionContext(ParserLimits limits) : parser(limits) {}
        IncrementalRtspParser parser;
        std::optional<audio::EncodedAudioFormat> audio_format{};
    };

    [[nodiscard]] transport::ControlReply HandleRequest(
        transport::ConnectionId connection_id,
        const Request& request);
    [[nodiscard]] std::vector<std::byte> StatusResponse(const Request& request,
                                                        int status,
                                                        std::string_view reason,
                                                        std::span<const Header> headers = {},
                                                        std::string_view body = {},
                                                        bool close = false) const;

    const crypto::ISessionAuthenticator& authenticator_;
    ParserLimits parser_limits_{};
    session::SessionManager sessions_;
    transport::IAudioTransportController* audio_transport_{nullptr};
    mutable std::mutex mutex_{};
    std::unordered_map<transport::ConnectionId, ConnectionContext> contexts_{};
    std::uint64_t received_requests_{0U};
    std::uint64_t parse_errors_{0U};
    std::uint64_t rejected_requests_{0U};
    std::uint64_t unsupported_requests_{0U};
    std::uint64_t pairing_requests_{0U};
    std::string last_error_{};
};

}  // namespace airplaywin::protocol
