#pragma once

#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/session/SessionTypes.h"

namespace airplaywin::session {

class SessionManager final {
public:
    explicit SessionManager(ActiveSessionPolicy policy = ActiveSessionPolicy::RejectNew);

    [[nodiscard]] bool Create(transport::ConnectionId connection_id,
                              std::string_view peer_address);
    [[nodiscard]] bool RecordRequest(transport::ConnectionId connection_id,
                                     std::string_view method,
                                     std::string_view sender_identity = {});
    [[nodiscard]] bool Transition(transport::ConnectionId connection_id,
                                  SessionState expected,
                                  SessionState next);
    [[nodiscard]] bool SetState(transport::ConnectionId connection_id, SessionState state);
    [[nodiscard]] bool SetVolume(transport::ConnectionId connection_id, double volume_db);
    [[nodiscard]] ActivationResult Activate(transport::ConnectionId connection_id);
    void Close(transport::ConnectionId connection_id) noexcept;

    [[nodiscard]] std::optional<SessionSnapshot> Get(
        transport::ConnectionId connection_id) const;
    [[nodiscard]] std::vector<SessionSnapshot> Snapshots() const;
    [[nodiscard]] SessionDiagnostics Diagnostics() const;

private:
    [[nodiscard]] static std::string BuildSessionId(transport::ConnectionId connection_id);

    mutable std::mutex mutex_{};
    ActiveSessionPolicy policy_{ActiveSessionPolicy::RejectNew};
    std::unordered_map<transport::ConnectionId, SessionSnapshot> sessions_{};
    std::optional<transport::ConnectionId> playback_owner_{};
    std::uint64_t created_sessions_{0U};
    std::uint64_t closed_sessions_{0U};
    std::uint64_t rejected_sessions_{0U};
    std::uint64_t preempted_sessions_{0U};
};

}  // namespace airplaywin::session
