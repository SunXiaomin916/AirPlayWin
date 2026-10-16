#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace airplaywin::transport {

using ConnectionId = std::uint64_t;

enum class DisconnectReason : std::uint8_t {
    PeerClosed,
    Requested,
    IdleTimeout,
    TransportError,
    ServerShutdown,
    ProtocolError,
};

struct ControlReply final {
    std::vector<std::vector<std::byte>> writes{};
    bool close_after_writes{false};
};

class IControlConnectionHandler {
public:
    virtual ~IControlConnectionHandler() = default;

    virtual void OnConnected(ConnectionId connection_id,
                             std::string_view peer_address,
                             std::string_view local_address) = 0;
    [[nodiscard]] virtual ControlReply OnBytes(ConnectionId connection_id,
                                               std::span<const std::byte> bytes) = 0;
    virtual void OnDisconnected(ConnectionId connection_id, DisconnectReason reason) = 0;
    // Veto for the transport's idle-timeout scan: return false to keep the
    // connection open even when no bytes arrived recently (e.g. a paused
    // sender whose session must stay alive). Connections are still subject
    // to the never-completed-request hard deadline.
    [[nodiscard]] virtual bool AllowIdleClose(ConnectionId connection_id) noexcept {
        static_cast<void>(connection_id);
        return true;
    }
    // True once the connection produced at least one fully parsed request.
    // Used to hard-close slow connections that never make progress (they
    // would otherwise refresh their idle timer one byte at a time).
    [[nodiscard]] virtual bool HasCompletedRequest(ConnectionId connection_id) const noexcept {
        static_cast<void>(connection_id);
        return true;
    }
};

}  // namespace airplaywin::transport
