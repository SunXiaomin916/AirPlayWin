#include "core/protocol/AirPlayControlService.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <string_view>

#include "core/protocol/RtspResponseBuilder.h"

namespace airplaywin::protocol {

namespace {

[[nodiscard]] bool RequestWantsClose(const Request& request) noexcept {
    if (const auto connection = request.HeaderValue("Connection"); connection.has_value()) {
        return EqualsAsciiCaseInsensitive(*connection, "close");
    }
    return request.version == ProtocolVersion::Http10;
}

[[nodiscard]] bool IsPairingTarget(const std::string_view target) noexcept {
    return target.starts_with("/pair-") || target == "/fp-setup" ||
           target.ends_with("/pair-setup") || target.ends_with("/pair-verify") ||
           target.ends_with("/fp-setup");
}

[[nodiscard]] std::string_view SenderIdentity(const Request& request) noexcept {
    if (const auto value = request.HeaderValue("DACP-ID"); value.has_value()) {
        return *value;
    }
    if (const auto value = request.HeaderValue("Active-Remote"); value.has_value()) {
        return *value;
    }
    if (const auto value = request.HeaderValue("User-Agent"); value.has_value()) {
        return *value;
    }
    return {};
}

[[nodiscard]] std::string_view Trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                             text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1U);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1U);
    }
    return text;
}

[[nodiscard]] std::optional<double> ParseVolume(const std::string_view body) noexcept {
    const auto colon = body.find(':');
    if (colon == std::string_view::npos ||
        !EqualsAsciiCaseInsensitive(Trim(body.substr(0U, colon)), "volume")) {
        return std::nullopt;
    }
    const auto value = Trim(body.substr(colon + 1U));
    double volume = 0.0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), volume);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        !std::isfinite(volume) || volume < -144.0 || volume > 0.0) {
        return std::nullopt;
    }
    return volume;
}

[[nodiscard]] bool HasValidCSeq(const Request& request) noexcept {
    std::size_t matches = 0U;
    std::string_view value;
    for (const auto& header : request.headers) {
        if (EqualsAsciiCaseInsensitive(header.name, "CSeq")) {
            ++matches;
            value = header.value;
        }
    }
    if (matches != 1U || value.empty() || value.front() == '+' || value.front() == '-') {
        return false;
    }
    std::uint64_t sequence = 0U;
    const auto result =
        std::from_chars(value.data(), value.data() + value.size(), sequence, 10);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

}  // namespace

AirPlayControlService::AirPlayControlService(
    const crypto::ISessionAuthenticator& authenticator,
    const session::ActiveSessionPolicy policy,
    const ParserLimits parser_limits)
    : authenticator_(authenticator), parser_limits_(parser_limits), sessions_(policy) {}

void AirPlayControlService::OnConnected(const transport::ConnectionId connection_id,
                                        const std::string_view peer_address) {
    std::scoped_lock lock{mutex_};
    const auto [iterator, inserted] = contexts_.try_emplace(connection_id, parser_limits_);
    if (!inserted || !sessions_.Create(connection_id, peer_address)) {
        if (inserted) {
            contexts_.erase(iterator);
        }
        last_error_ = "duplicate or invalid control connection identifier";
    }
}

transport::ControlReply AirPlayControlService::OnBytes(
    const transport::ConnectionId connection_id,
    const std::span<const std::byte> bytes) {
    std::scoped_lock lock{mutex_};
    const auto context = contexts_.find(connection_id);
    if (context == contexts_.end()) {
        ++rejected_requests_;
        last_error_ = "received bytes for an unknown control connection";
        return {.writes = {}, .close_after_writes = true};
    }
    auto batch = context->second.parser.Feed(bytes);
    transport::ControlReply reply;
    if (batch.error.has_value()) {
        ++parse_errors_;
        last_error_ = batch.error->detail;
        Request synthetic;
        reply.writes.push_back(StatusResponse(synthetic, 400, "Bad Request", {}, {}, true));
        reply.close_after_writes = true;
        static_cast<void>(sessions_.SetState(connection_id, session::SessionState::Error));
        return reply;
    }
    for (const auto& request : batch.requests) {
        ++received_requests_;
        static_cast<void>(sessions_.RecordRequest(connection_id, request.method,
                                                  SenderIdentity(request)));
        auto response = HandleRequest(connection_id, request);
        for (auto& write : response.writes) {
            reply.writes.push_back(std::move(write));
        }
        if (response.close_after_writes) {
            reply.close_after_writes = true;
            break;
        }
    }
    return reply;
}

transport::ControlReply AirPlayControlService::HandleRequest(
    const transport::ConnectionId connection_id,
    const Request& request) {
    transport::ControlReply reply;
    const bool close = RequestWantsClose(request);
    if (request.version == ProtocolVersion::Rtsp10 && !HasValidCSeq(request)) {
        ++rejected_requests_;
        reply.writes.push_back(StatusResponse(request, 400, "Bad Request", {}, {}, true));
        reply.close_after_writes = true;
        return reply;
    }

    if (IsPairingTarget(request.target)) {
        ++pairing_requests_;
        ++unsupported_requests_;
        reply.writes.push_back(StatusResponse(request, 501, "Not Implemented", {}, {}, close));
        reply.close_after_writes = close;
        return reply;
    }

    const auto authorization = authenticator_.Authorize(request);
    if (authorization.result != crypto::AuthorizationResult::Allowed) {
        ++rejected_requests_;
        if (authorization.result == crypto::AuthorizationResult::AuthenticationRequired) {
            const std::array headers{Header{"WWW-Authenticate", authorization.challenge}};
            reply.writes.push_back(
                StatusResponse(request, 401, "Unauthorized", headers, {}, close));
        } else {
            reply.writes.push_back(StatusResponse(request, 403, "Forbidden", {}, {}, close));
        }
        reply.close_after_writes = close;
        return reply;
    }

    if (EqualsAsciiCaseInsensitive(request.method, "OPTIONS")) {
        const std::array headers{
            Header{"Public", "ANNOUNCE, OPTIONS, GET_PARAMETER, SET_PARAMETER, TEARDOWN"}};
        reply.writes.push_back(StatusResponse(request, 200, "OK", headers, {}, close));
    } else if (EqualsAsciiCaseInsensitive(request.method, "ANNOUNCE")) {
        const auto content_type = request.HeaderValue("Content-Type");
        if (!content_type.has_value() ||
            !EqualsAsciiCaseInsensitive(*content_type, "application/sdp")) {
            ++rejected_requests_;
            reply.writes.push_back(
                StatusResponse(request, 415, "Unsupported Media Type", {}, {}, close));
        } else if (request.body.empty() || request.BodyText().find("m=audio") ==
                                               std::string_view::npos) {
            ++rejected_requests_;
            reply.writes.push_back(StatusResponse(request, 400, "Bad Request", {}, {}, close));
        } else {
            const auto activation = sessions_.Activate(connection_id);
            if (activation == session::ActivationResult::Rejected) {
                ++rejected_requests_;
                reply.writes.push_back(
                    StatusResponse(request, 453, "Not Enough Bandwidth", {}, {}, close));
            } else {
                static_cast<void>(
                    sessions_.SetState(connection_id, session::SessionState::Announced));
                const auto snapshot = sessions_.Get(connection_id);
                const std::array headers{Header{
                    "Session", snapshot.has_value() ? snapshot->session_id : std::string{}}};
                reply.writes.push_back(StatusResponse(request, 200, "OK", headers, {}, close));
            }
        }
    } else if (EqualsAsciiCaseInsensitive(request.method, "GET_PARAMETER")) {
        reply.writes.push_back(StatusResponse(request, 200, "OK", {}, {}, close));
    } else if (EqualsAsciiCaseInsensitive(request.method, "SET_PARAMETER")) {
        const auto content_type = request.HeaderValue("Content-Type");
        const auto volume = content_type.has_value() &&
                                    EqualsAsciiCaseInsensitive(*content_type, "text/parameters")
                                ? ParseVolume(request.BodyText())
                                : std::nullopt;
        if (!volume.has_value()) {
            ++rejected_requests_;
            reply.writes.push_back(
                StatusResponse(request, 451, "Invalid Parameter", {}, {}, close));
        } else {
            static_cast<void>(sessions_.SetVolume(connection_id, *volume));
            reply.writes.push_back(StatusResponse(request, 200, "OK", {}, {}, close));
        }
    } else if (EqualsAsciiCaseInsensitive(request.method, "TEARDOWN")) {
        static_cast<void>(sessions_.SetState(connection_id, session::SessionState::Closing));
        reply.writes.push_back(StatusResponse(request, 200, "OK", {}, {}, true));
        reply.close_after_writes = true;
        return reply;
    } else if (EqualsAsciiCaseInsensitive(request.method, "SETUP")) {
        ++unsupported_requests_;
        reply.writes.push_back(
            StatusResponse(request, 461, "Unsupported Transport", {}, {}, close));
    } else if (EqualsAsciiCaseInsensitive(request.method, "RECORD") ||
               EqualsAsciiCaseInsensitive(request.method, "PAUSE") ||
               EqualsAsciiCaseInsensitive(request.method, "FLUSH")) {
        ++unsupported_requests_;
        reply.writes.push_back(
            StatusResponse(request, 455, "Method Not Valid in This State", {}, {}, close));
    } else {
        ++unsupported_requests_;
        reply.writes.push_back(StatusResponse(request, 501, "Not Implemented", {}, {}, close));
    }
    reply.close_after_writes = close;
    return reply;
}

void AirPlayControlService::OnDisconnected(const transport::ConnectionId connection_id,
                                           const transport::DisconnectReason reason) {
    static_cast<void>(reason);
    std::scoped_lock lock{mutex_};
    contexts_.erase(connection_id);
    sessions_.Close(connection_id);
}

ControlDiagnostics AirPlayControlService::Diagnostics() const {
    std::scoped_lock lock{mutex_};
    return ControlDiagnostics{
        .sessions = sessions_.Diagnostics(),
        .received_requests = received_requests_,
        .parse_errors = parse_errors_,
        .rejected_requests = rejected_requests_,
        .unsupported_requests = unsupported_requests_,
        .pairing_requests = pairing_requests_,
        .last_error = last_error_,
    };
}

std::vector<session::SessionSnapshot> AirPlayControlService::Sessions() const {
    std::scoped_lock lock{mutex_};
    return sessions_.Snapshots();
}

std::vector<std::byte> AirPlayControlService::StatusResponse(
    const Request& request,
    const int status,
    const std::string_view reason,
    const std::span<const Header> headers,
    const std::string_view body,
    const bool close) const {
    const auto bytes = std::span<const std::byte>{reinterpret_cast<const std::byte*>(body.data()),
                                                  body.size()};
    return BuildResponse(request, ResponseSpec{.status_code = status,
                                               .reason = reason,
                                               .headers = headers,
                                               .body = bytes,
                                               .close_connection = close});
}

}  // namespace airplaywin::protocol
