#include "core/protocol/AirPlayControlService.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <string_view>

#include "core/protocol/RtspResponseBuilder.h"
#include "core/protocol/RtpInfo.h"
#include "core/protocol/RtspTransport.h"
#include "core/protocol/SdpAudioParser.h"

namespace airplaywin::protocol {

namespace {

constexpr std::size_t kRecentRequestTraceCapacity = 16U;

[[nodiscard]] std::string BoundedCopy(const std::string_view value,
                                      const std::size_t maximum_bytes) {
    return std::string{value.substr(0U, maximum_bytes)};
}

[[nodiscard]] int ResponseStatus(const transport::ControlReply& reply) noexcept {
    if (reply.writes.empty() || reply.writes.front().empty()) {
        return 0;
    }
    const auto& bytes = reply.writes.front();
    const std::string_view response{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    const auto first_space = response.find(' ');
    if (first_space == std::string_view::npos) {
        return 0;
    }
    const auto status_begin = first_space + 1U;
    const auto status_end = response.find(' ', status_begin);
    if (status_end == std::string_view::npos) {
        return 0;
    }
    int status = 0;
    const auto result = std::from_chars(response.data() + status_begin,
                                        response.data() + status_end, status, 10);
    return result.ec == std::errc{} && result.ptr == response.data() + status_end ? status : 0;
}

[[nodiscard]] bool ResponseHasHeader(const transport::ControlReply& reply,
                                     const std::string_view header_name) noexcept {
    if (reply.writes.empty() || reply.writes.front().empty()) {
        return false;
    }
    const auto& bytes = reply.writes.front();
    const std::string_view response{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    return response.find("\r\n" + std::string{header_name} + ": ") != std::string_view::npos;
}

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

[[nodiscard]] bool HasBinaryPlistEnvelope(const Request& request) noexcept {
    constexpr std::array<std::byte, 8U> kHeader{
        std::byte{'b'}, std::byte{'p'}, std::byte{'l'}, std::byte{'i'},
        std::byte{'s'}, std::byte{'t'}, std::byte{'0'}, std::byte{'0'},
    };
    return request.body.size() >= kHeader.size() &&
           std::ranges::equal(kHeader, std::span{request.body}.first(kHeader.size()));
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
    std::optional<double> volume;
    std::size_t offset = 0U;
    while (offset <= body.size()) {
        const auto line_end = body.find('\n', offset);
        const auto line = Trim(body.substr(
            offset, line_end == std::string_view::npos ? std::string_view::npos
                                                       : line_end - offset));
        const auto colon = line.find(':');
        if (colon != std::string_view::npos &&
            EqualsAsciiCaseInsensitive(Trim(line.substr(0U, colon)), "volume")) {
            if (volume.has_value()) {
                return std::nullopt;
            }
            const auto value = Trim(line.substr(colon + 1U));
            double parsed = 0.0;
            const auto result =
                std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
                !std::isfinite(parsed) || parsed < -144.0 || parsed > 0.0) {
                return std::nullopt;
            }
            volume = parsed;
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        offset = line_end + 1U;
    }
    return volume;
}

[[nodiscard]] bool RequestsVolume(const std::string_view body) noexcept {
    std::size_t offset = 0U;
    while (offset <= body.size()) {
        const auto line_end = body.find('\n', offset);
        const auto line = Trim(body.substr(
            offset, line_end == std::string_view::npos ? std::string_view::npos
                                                       : line_end - offset));
        if (EqualsAsciiCaseInsensitive(line, "volume")) {
            return true;
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        offset = line_end + 1U;
    }
    return false;
}

[[nodiscard]] bool HasParameterAssignment(const std::string_view body,
                                          const std::string_view name) noexcept {
    std::size_t offset = 0U;
    while (offset <= body.size()) {
        const auto line_end = body.find('\n', offset);
        const auto line = Trim(body.substr(
            offset, line_end == std::string_view::npos ? std::string_view::npos
                                                       : line_end - offset));
        const auto colon = line.find(':');
        if (colon != std::string_view::npos &&
            EqualsAsciiCaseInsensitive(Trim(line.substr(0U, colon)), name)) {
            return true;
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        offset = line_end + 1U;
    }
    return false;
}

[[nodiscard]] bool IsMetadataParameterContentType(const std::string_view value) noexcept {
    return EqualsAsciiCaseInsensitive(value, "application/x-dmap-tagged") ||
           EqualsAsciiCaseInsensitive(value, "image/jpeg") ||
           EqualsAsciiCaseInsensitive(value, "image/png") ||
           EqualsAsciiCaseInsensitive(value, "image/none");
}

[[nodiscard]] std::string VolumeBody(const double volume_db) {
    std::array<char, 64U> storage{};
    constexpr std::string_view prefix{"volume: "};
    std::copy(prefix.begin(), prefix.end(), storage.begin());
    const auto result = std::to_chars(storage.data() + prefix.size(), storage.data() + 62U,
                                      volume_db, std::chars_format::fixed, 6);
    if (result.ec != std::errc{}) {
        return {};
    }
    *result.ptr = '\r';
    *(result.ptr + 1) = '\n';
    return {storage.data(), static_cast<std::size_t>(result.ptr + 2 - storage.data())};
}

[[nodiscard]] bool ParseOptionalRtpInfo(
    const Request& request,
    transport::AudioTimelineAnchor& anchor) noexcept {
    std::optional<std::string_view> value;
    for (const auto& header : request.headers) {
        if (EqualsAsciiCaseInsensitive(header.name, "RTP-Info")) {
            if (value.has_value()) {
                return false;
            }
            value = header.value;
        }
    }
    if (!value.has_value()) {
        anchor = {};
        return true;
    }
    const auto parsed = ParseRtpInfo(*value);
    if (!parsed.has_value()) {
        return false;
    }
    anchor = *parsed;
    return true;
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
    const ParserLimits parser_limits,
    transport::IAudioTransportController* const audio_transport,
    const crypto::IRaopCryptoProvider* const raop_crypto)
    : authenticator_(authenticator),
      parser_limits_(parser_limits),
      sessions_(policy),
      audio_transport_(audio_transport),
      raop_crypto_(raop_crypto) {}

void AirPlayControlService::OnConnected(const transport::ConnectionId connection_id,
                                        const std::string_view peer_address,
                                        const std::string_view local_address) {
    std::scoped_lock lock{mutex_};
    const auto [iterator, inserted] =
        contexts_.try_emplace(connection_id, parser_limits_, local_address);
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
        RecordRequestTrace(connection_id, request, response);
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

    if (EqualsAsciiCaseInsensitive(request.method, "POST") && request.target == "/command") {
        const auto content_type = request.HeaderValue("Content-Type");
        if (!content_type.has_value() || !EqualsAsciiCaseInsensitive(
                                             *content_type,
                                             "application/x-apple-binary-plist")) {
            ++rejected_requests_;
            reply.writes.push_back(
                StatusResponse(request, 415, "Unsupported Media Type", {}, {}, close));
        } else if (!HasBinaryPlistEnvelope(request)) {
            ++rejected_requests_;
            reply.writes.push_back(StatusResponse(request, 400, "Bad Request", {}, {}, close));
        } else {
            // AirPlay 2 senders use /command for metadata and playback-state updates. The
            // audio path does not depend on those values, so acknowledge a structurally
            // valid plist without retaining its potentially sensitive body.
            reply.writes.push_back(StatusResponse(request, 200, "OK", {}, {}, close));
        }
    } else if (EqualsAsciiCaseInsensitive(request.method, "OPTIONS")) {
        std::vector<Header> headers;
        headers.push_back(Header{
            "Public", audio_transport_ != nullptr
                          ? "ANNOUNCE, OPTIONS, SETUP, RECORD, PAUSE, FLUSH, GET_PARAMETER, "
                            "SET_PARAMETER, TEARDOWN"
                          : "ANNOUNCE, OPTIONS, GET_PARAMETER, SET_PARAMETER, TEARDOWN"});
        if (const auto challenge = request.HeaderValue("Apple-Challenge");
            challenge.has_value() && raop_crypto_ != nullptr) {
            const auto& context = contexts_.at(connection_id);
            const auto response =
                raop_crypto_->BuildAppleResponse(*challenge, context.local_address);
            if (!response.has_value()) {
                ++rejected_requests_;
                reply.writes.push_back(
                    StatusResponse(request, 500, "Internal Server Error", {}, {}, close));
                reply.close_after_writes = close;
                return reply;
            }
            headers.push_back(Header{"Apple-Response", *response});
            headers.push_back(Header{"Audio-Jack-Status", "connected; type=analog"});
        }
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
            const auto parsed_audio = ParseSdpAudioSession(request.BodyText());
            if (audio_transport_ != nullptr && !parsed_audio.has_value()) {
                ++rejected_requests_;
                reply.writes.push_back(
                    StatusResponse(request, 415, "Unsupported Media Type", {}, {}, close));
                reply.close_after_writes = close;
                return reply;
            }
            std::optional<audio::EncodedAudioFormat> negotiated_format;
            if (parsed_audio.has_value()) {
                negotiated_format = parsed_audio->format;
                if (parsed_audio->encrypted_aes_key.has_value()) {
                    const auto material =
                        raop_crypto_ != nullptr
                            ? raop_crypto_->DecryptAesMaterial(
                                  *parsed_audio->encrypted_aes_key,
                                  *parsed_audio->aes_initialization_vector)
                            : std::nullopt;
                    if (!material.has_value()) {
                        ++rejected_requests_;
                        reply.writes.push_back(StatusResponse(
                            request, 400, "Bad Request", {}, {}, close));
                        reply.close_after_writes = close;
                        return reply;
                    }
                    negotiated_format->encryption_key = material->key;
                    negotiated_format->encryption_iv = material->iv;
                }
            }
            const auto activation = sessions_.Activate(connection_id);
            if (activation == session::ActivationResult::Rejected) {
                ++rejected_requests_;
                reply.writes.push_back(
                    StatusResponse(request, 453, "Not Enough Bandwidth", {}, {}, close));
            } else {
                if (negotiated_format.has_value()) {
                    contexts_.at(connection_id).audio_format = *negotiated_format;
                }
                static_cast<void>(
                    sessions_.SetState(connection_id, session::SessionState::Announced));
                const auto snapshot = sessions_.Get(connection_id);
                const std::array headers{Header{
                    "Session", snapshot.has_value() ? snapshot->session_id : std::string{}}};
                reply.writes.push_back(StatusResponse(request, 200, "OK", headers, {}, close));
            }
        }
    } else if (EqualsAsciiCaseInsensitive(request.method, "GET_PARAMETER")) {
        if (request.body.empty()) {
            reply.writes.push_back(StatusResponse(request, 200, "OK", {}, {}, close));
        } else {
            const auto content_type = request.HeaderValue("Content-Type");
            const auto session_snapshot = sessions_.Get(connection_id);
            if (!content_type.has_value() ||
                !EqualsAsciiCaseInsensitive(*content_type, "text/parameters") ||
                !RequestsVolume(request.BodyText()) || !session_snapshot.has_value()) {
                ++rejected_requests_;
                reply.writes.push_back(
                    StatusResponse(request, 451, "Invalid Parameter", {}, {}, close));
            } else {
                const auto body = VolumeBody(session_snapshot->volume_db);
                const std::array headers{Header{"Content-Type", "text/parameters"}};
                reply.writes.push_back(
                    StatusResponse(request, 200, "OK", headers, body, close));
            }
        }
    } else if (EqualsAsciiCaseInsensitive(request.method, "SET_PARAMETER")) {
        const auto content_type = request.HeaderValue("Content-Type");
        const auto is_text_parameters =
            content_type.has_value() &&
            EqualsAsciiCaseInsensitive(*content_type, "text/parameters");
        const auto has_volume =
            is_text_parameters && HasParameterAssignment(request.BodyText(), "volume");
        const auto volume = has_volume ? ParseVolume(request.BodyText()) : std::nullopt;
        const auto is_metadata =
            content_type.has_value() && IsMetadataParameterContentType(*content_type);
        if (!content_type.has_value() || (has_volume && !volume.has_value()) ||
            (!is_text_parameters && !is_metadata)) {
            ++rejected_requests_;
            reply.writes.push_back(
                StatusResponse(request, 451, "Invalid Parameter", {}, {}, close));
        } else {
            if (volume.has_value()) {
                static_cast<void>(sessions_.SetVolume(connection_id, *volume));
                if (audio_transport_ != nullptr) {
                    const auto gain =
                        *volume <= -144.0
                            ? 0.0F
                            : static_cast<float>(std::pow(10.0, *volume / 20.0));
                    audio_transport_->SetVolume(connection_id, gain);
                }
            }
            // Progress, DMAP metadata, and artwork are control-plane hints.  They
            // are acknowledged for sender compatibility but deliberately not
            // retained, logged, or passed into the audio path yet.
            reply.writes.push_back(StatusResponse(request, 200, "OK", {}, {}, close));
        }
    } else if (EqualsAsciiCaseInsensitive(request.method, "TEARDOWN")) {
        if (audio_transport_ != nullptr) {
            audio_transport_->Teardown(connection_id);
        }
        static_cast<void>(sessions_.SetState(connection_id, session::SessionState::Closing));
        reply.writes.push_back(StatusResponse(request, 200, "OK", {}, {}, true));
        reply.close_after_writes = true;
        return reply;
    } else if (EqualsAsciiCaseInsensitive(request.method, "SETUP")) {
        const auto session_snapshot = sessions_.Get(connection_id);
        const auto transport_header = request.HeaderValue("Transport");
        const auto& context = contexts_.at(connection_id);
        if (audio_transport_ == nullptr) {
            ++unsupported_requests_;
            reply.writes.push_back(
                StatusResponse(request, 461, "Unsupported Transport", {}, {}, close));
        } else if (!session_snapshot.has_value() ||
                   session_snapshot->state != session::SessionState::Announced ||
                   !context.audio_format.has_value()) {
            ++rejected_requests_;
            reply.writes.push_back(StatusResponse(
                request, 455, "Method Not Valid in This State", {}, {}, close));
        } else if (!transport_header.has_value()) {
            ++rejected_requests_;
            reply.writes.push_back(StatusResponse(request, 400, "Bad Request", {}, {}, close));
        } else {
            const auto parsed_transport = ParseRecordTransport(*transport_header);
            if (!parsed_transport.has_value()) {
                ++rejected_requests_;
                reply.writes.push_back(
                    StatusResponse(request, 461, "Unsupported Transport", {}, {}, close));
            } else {
                const auto setup = audio_transport_->Setup(
                    transport::AudioTransportSetupRequest{
                        .connection_id = connection_id,
                        .peer_address = session_snapshot->peer_address,
                        .format = *context.audio_format,
                        .protocol_latency_frames =
                            transport::kClassicDevelopmentAudioLatencyFrames,
                        .client_control_port = parsed_transport->client_control_port,
                        .client_timing_port = parsed_transport->client_timing_port,
                    });
                if (!setup.Succeeded()) {
                    ++rejected_requests_;
                    reply.writes.push_back(
                        StatusResponse(request, 500, "Internal Server Error", {}, {}, close));
                } else {
                    const auto response_transport = BuildRecordTransportResponse(
                        setup.server_audio_port, setup.server_control_port,
                        setup.server_timing_port);
                    const auto initial_gain = session_snapshot->volume_db <= -144.0
                                                  ? 0.0F
                                                  : static_cast<float>(std::pow(
                                                        10.0,
                                                        session_snapshot->volume_db / 20.0));
                    audio_transport_->SetVolume(connection_id, initial_gain);
                    const std::array headers{
                        Header{"Transport", response_transport},
                        Header{"Session", session_snapshot->session_id},
                    };
                    static_cast<void>(sessions_.SetStream(
                        connection_id, context.audio_format->sample_rate,
                        context.audio_format->channel_count, context.audio_format->payload_type,
                        setup.server_audio_port, setup.server_control_port,
                        setup.server_timing_port));
                    static_cast<void>(
                        sessions_.SetState(connection_id, session::SessionState::Ready));
                    reply.writes.push_back(
                        StatusResponse(request, 200, "OK", headers, {}, close));
                }
            }
        }
    } else if (EqualsAsciiCaseInsensitive(request.method, "RECORD")) {
        const auto session_snapshot = sessions_.Get(connection_id);
        transport::AudioTimelineAnchor anchor;
        if (audio_transport_ == nullptr || !session_snapshot.has_value() ||
            (session_snapshot->state != session::SessionState::Ready &&
             session_snapshot->state != session::SessionState::Paused)) {
            ++rejected_requests_;
            reply.writes.push_back(StatusResponse(
                request, 455, "Method Not Valid in This State", {}, {}, close));
        } else if (!ParseOptionalRtpInfo(request, anchor)) {
            ++rejected_requests_;
            reply.writes.push_back(
                StatusResponse(request, 400, "Bad Request", {}, {}, close));
        } else {
            const bool started = session_snapshot->state == session::SessionState::Paused
                                     ? audio_transport_->Resume(connection_id, anchor)
                                     : audio_transport_->Record(connection_id, anchor);
            if (!started) {
                ++rejected_requests_;
                reply.writes.push_back(
                    StatusResponse(request, 500, "Internal Server Error", {}, {}, close));
            } else {
                static_cast<void>(
                    sessions_.SetState(connection_id, session::SessionState::Streaming));
                const std::array headers{Header{
                    "Audio-Latency",
                    std::to_string(transport::kClassicDevelopmentAudioLatencyFrames)}};
                reply.writes.push_back(
                    StatusResponse(request, 200, "OK", headers, {}, close));
            }
        }
    } else if (EqualsAsciiCaseInsensitive(request.method, "PAUSE")) {
        const auto session_snapshot = sessions_.Get(connection_id);
        if (audio_transport_ == nullptr || !session_snapshot.has_value() ||
            session_snapshot->state != session::SessionState::Streaming ||
            !audio_transport_->Pause(connection_id)) {
            ++rejected_requests_;
            reply.writes.push_back(StatusResponse(
                request, 455, "Method Not Valid in This State", {}, {}, close));
        } else {
            static_cast<void>(sessions_.SetState(connection_id, session::SessionState::Paused));
            reply.writes.push_back(StatusResponse(request, 200, "OK", {}, {}, close));
        }
    } else if (EqualsAsciiCaseInsensitive(request.method, "FLUSH")) {
        const auto session_snapshot = sessions_.Get(connection_id);
        transport::AudioTimelineAnchor anchor;
        if (audio_transport_ == nullptr || !session_snapshot.has_value() ||
            (session_snapshot->state != session::SessionState::Ready &&
             session_snapshot->state != session::SessionState::Streaming &&
             session_snapshot->state != session::SessionState::Paused)) {
            ++rejected_requests_;
            reply.writes.push_back(StatusResponse(
                request, 455, "Method Not Valid in This State", {}, {}, close));
        } else if (!ParseOptionalRtpInfo(request, anchor)) {
            ++rejected_requests_;
            reply.writes.push_back(
                StatusResponse(request, 400, "Bad Request", {}, {}, close));
        } else if (!audio_transport_->Flush(connection_id, anchor)) {
            ++rejected_requests_;
            reply.writes.push_back(
                StatusResponse(request, 500, "Internal Server Error", {}, {}, close));
        } else {
            reply.writes.push_back(StatusResponse(request, 200, "OK", {}, {}, close));
        }
    } else {
        ++unsupported_requests_;
        reply.writes.push_back(StatusResponse(request, 501, "Not Implemented", {}, {}, close));
    }
    reply.close_after_writes = close;
    return reply;
}

void AirPlayControlService::OnDisconnected(const transport::ConnectionId connection_id,
                                           const transport::DisconnectReason reason) {
    std::scoped_lock lock{mutex_};
    switch (reason) {
    case transport::DisconnectReason::PeerClosed:
        ++peer_disconnects_;
        break;
    case transport::DisconnectReason::Requested:
        ++requested_disconnects_;
        break;
    case transport::DisconnectReason::IdleTimeout:
        ++idle_disconnects_;
        break;
    case transport::DisconnectReason::TransportError:
        ++transport_disconnects_;
        break;
    case transport::DisconnectReason::ServerShutdown:
        ++shutdown_disconnects_;
        break;
    case transport::DisconnectReason::ProtocolError:
        ++protocol_disconnects_;
        break;
    }
    if (audio_transport_ != nullptr) {
        audio_transport_->Teardown(connection_id);
    }
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
        .peer_disconnects = peer_disconnects_,
        .requested_disconnects = requested_disconnects_,
        .idle_disconnects = idle_disconnects_,
        .transport_disconnects = transport_disconnects_,
        .shutdown_disconnects = shutdown_disconnects_,
        .protocol_disconnects = protocol_disconnects_,
        .transport = audio_transport_ != nullptr
                         ? audio_transport_->Diagnostics()
                         : transport::AudioTransportDiagnostics{},
        .recent_requests = recent_requests_,
        .last_error = last_error_,
    };
}

void AirPlayControlService::RecordRequestTrace(
    const transport::ConnectionId connection_id,
    const Request& request,
    const transport::ControlReply& reply) {
    if (recent_requests_.size() == kRecentRequestTraceCapacity) {
        recent_requests_.erase(recent_requests_.begin());
    }
    const auto content_type = request.HeaderValue("Content-Type");
    const bool apple_challenge_present = request.HeaderValue("Apple-Challenge").has_value();
    recent_requests_.push_back(ControlRequestTrace{
        .sequence = received_requests_,
        .connection_id = connection_id,
        .method = BoundedCopy(request.method, 32U),
        .target = BoundedCopy(request.target, 256U),
        .protocol = std::string{ToString(request.version)},
        .content_type = content_type.has_value() ? BoundedCopy(*content_type, 128U)
                                                 : std::string{},
        .body_bytes = request.body.size(),
        .response_status = ResponseStatus(reply),
        .apple_challenge_present = apple_challenge_present,
        .apple_response_sent = ResponseHasHeader(reply, "Apple-Response"),
    });
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
                                               .close_connection = close,
                                               .server_name = raop_crypto_ != nullptr
                                                                  ? "AirTunes/105.1"
                                                                  : "AirPlayWin/"
                                                                        AIRPLAYWIN_VERSION_STRING});
}

}  // namespace airplaywin::protocol
