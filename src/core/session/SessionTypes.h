#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/transport/IControlConnectionHandler.h"

namespace airplaywin::session {

enum class SessionState : std::uint8_t {
    Connected,
    Announced,
    Ready,
    Streaming,
    Paused,
    Closing,
    Closed,
    Error,
};

enum class ActiveSessionPolicy : std::uint8_t {
    RejectNew,
    PreemptExisting,
};

enum class ActivationResult : std::uint8_t {
    Activated,
    AlreadyActive,
    Rejected,
    PreemptedExisting,
    UnknownSession,
};

struct SessionSnapshot final {
    transport::ConnectionId connection_id{0U};
    std::string session_id{};
    std::string peer_address{};
    std::string sender_identity{};
    SessionState state{SessionState::Connected};
    std::uint64_t request_count{0U};
    std::string last_method{};
    double volume_db{0.0};
    std::uint32_t stream_sample_rate{0U};
    std::uint16_t stream_channel_count{0U};
    std::uint8_t stream_payload_type{0U};
    std::uint16_t server_audio_port{0U};
    std::uint16_t server_control_port{0U};
    std::uint16_t server_timing_port{0U};
    bool owns_playback{false};
    std::chrono::steady_clock::time_point last_activity{};
};

struct SessionDiagnostics final {
    std::size_t tracked_sessions{0U};
    std::size_t active_connections{0U};
    std::optional<transport::ConnectionId> playback_owner{};
    std::uint64_t created_sessions{0U};
    std::uint64_t closed_sessions{0U};
    std::uint64_t rejected_sessions{0U};
    std::uint64_t preempted_sessions{0U};
};

}  // namespace airplaywin::session
