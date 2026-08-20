#include "core/session/SessionManager.h"

#include <charconv>
#include <string>

namespace airplaywin::session {

SessionManager::SessionManager(const ActiveSessionPolicy policy) : policy_(policy) {}

bool SessionManager::Create(const transport::ConnectionId connection_id,
                            const std::string_view peer_address) {
    if (connection_id == 0U) {
        return false;
    }
    std::scoped_lock lock{mutex_};
    auto [iterator, inserted] = sessions_.try_emplace(connection_id);
    if (!inserted) {
        return false;
    }
    iterator->second.connection_id = connection_id;
    iterator->second.session_id = BuildSessionId(connection_id);
    iterator->second.peer_address.assign(peer_address);
    iterator->second.last_activity = std::chrono::steady_clock::now();
    ++created_sessions_;
    return true;
}

bool SessionManager::RecordRequest(const transport::ConnectionId connection_id,
                                   const std::string_view method,
                                   const std::string_view sender_identity) {
    std::scoped_lock lock{mutex_};
    const auto iterator = sessions_.find(connection_id);
    if (iterator == sessions_.end()) {
        return false;
    }
    ++iterator->second.request_count;
    iterator->second.last_method.assign(method);
    if (!sender_identity.empty()) {
        iterator->second.sender_identity.assign(sender_identity);
    }
    iterator->second.last_activity = std::chrono::steady_clock::now();
    return true;
}

bool SessionManager::Transition(const transport::ConnectionId connection_id,
                                const SessionState expected,
                                const SessionState next) {
    std::scoped_lock lock{mutex_};
    const auto iterator = sessions_.find(connection_id);
    if (iterator == sessions_.end() || iterator->second.state != expected) {
        return false;
    }
    iterator->second.state = next;
    iterator->second.last_activity = std::chrono::steady_clock::now();
    return true;
}

bool SessionManager::SetState(const transport::ConnectionId connection_id,
                              const SessionState state) {
    std::scoped_lock lock{mutex_};
    const auto iterator = sessions_.find(connection_id);
    if (iterator == sessions_.end()) {
        return false;
    }
    iterator->second.state = state;
    iterator->second.last_activity = std::chrono::steady_clock::now();
    return true;
}

bool SessionManager::SetVolume(const transport::ConnectionId connection_id,
                               const double volume_db) {
    std::scoped_lock lock{mutex_};
    const auto iterator = sessions_.find(connection_id);
    if (iterator == sessions_.end()) {
        return false;
    }
    iterator->second.volume_db = volume_db;
    iterator->second.last_activity = std::chrono::steady_clock::now();
    return true;
}

bool SessionManager::SetStream(const transport::ConnectionId connection_id,
                               const std::uint32_t sample_rate,
                               const std::uint16_t channel_count,
                               const std::uint8_t payload_type,
                               const std::uint16_t audio_port,
                               const std::uint16_t control_port,
                               const std::uint16_t timing_port) {
    std::scoped_lock lock{mutex_};
    const auto iterator = sessions_.find(connection_id);
    if (iterator == sessions_.end()) {
        return false;
    }
    iterator->second.stream_sample_rate = sample_rate;
    iterator->second.stream_channel_count = channel_count;
    iterator->second.stream_payload_type = payload_type;
    iterator->second.server_audio_port = audio_port;
    iterator->second.server_control_port = control_port;
    iterator->second.server_timing_port = timing_port;
    iterator->second.last_activity = std::chrono::steady_clock::now();
    return true;
}

ActivationResult SessionManager::Activate(const transport::ConnectionId connection_id) {
    std::scoped_lock lock{mutex_};
    const auto requested = sessions_.find(connection_id);
    if (requested == sessions_.end()) {
        return ActivationResult::UnknownSession;
    }
    if (playback_owner_ == connection_id) {
        return ActivationResult::AlreadyActive;
    }
    if (!playback_owner_.has_value()) {
        playback_owner_ = connection_id;
        requested->second.owns_playback = true;
        return ActivationResult::Activated;
    }
    if (policy_ == ActiveSessionPolicy::RejectNew) {
        ++rejected_sessions_;
        return ActivationResult::Rejected;
    }
    const auto previous = sessions_.find(*playback_owner_);
    if (previous != sessions_.end()) {
        previous->second.owns_playback = false;
        previous->second.state = SessionState::Closing;
    }
    playback_owner_ = connection_id;
    requested->second.owns_playback = true;
    ++preempted_sessions_;
    return ActivationResult::PreemptedExisting;
}

void SessionManager::Close(const transport::ConnectionId connection_id) noexcept {
    std::scoped_lock lock{mutex_};
    const auto iterator = sessions_.find(connection_id);
    if (iterator == sessions_.end()) {
        return;
    }
    if (playback_owner_ == connection_id) {
        playback_owner_.reset();
    }
    sessions_.erase(iterator);
    ++closed_sessions_;
}

std::optional<SessionSnapshot> SessionManager::Get(
    const transport::ConnectionId connection_id) const {
    std::scoped_lock lock{mutex_};
    const auto iterator = sessions_.find(connection_id);
    if (iterator == sessions_.end()) {
        return std::nullopt;
    }
    return iterator->second;
}

std::vector<SessionSnapshot> SessionManager::Snapshots() const {
    std::scoped_lock lock{mutex_};
    std::vector<SessionSnapshot> snapshots;
    snapshots.reserve(sessions_.size());
    for (const auto& [connection_id, session] : sessions_) {
        static_cast<void>(connection_id);
        snapshots.push_back(session);
    }
    return snapshots;
}

SessionDiagnostics SessionManager::Diagnostics() const {
    std::scoped_lock lock{mutex_};
    return SessionDiagnostics{
        .tracked_sessions = sessions_.size(),
        .active_connections = sessions_.size(),
        .playback_owner = playback_owner_,
        .created_sessions = created_sessions_,
        .closed_sessions = closed_sessions_,
        .rejected_sessions = rejected_sessions_,
        .preempted_sessions = preempted_sessions_,
    };
}

std::string SessionManager::BuildSessionId(const transport::ConnectionId connection_id) {
    char storage[32]{};
    const auto result = std::to_chars(storage, storage + sizeof(storage), connection_id, 16);
    return {storage, static_cast<std::size_t>(result.ptr - storage)};
}

}  // namespace airplaywin::session
