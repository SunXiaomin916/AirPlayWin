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

    virtual void OnConnected(ConnectionId connection_id, std::string_view peer_address) = 0;
    [[nodiscard]] virtual ControlReply OnBytes(ConnectionId connection_id,
                                               std::span<const std::byte> bytes) = 0;
    virtual void OnDisconnected(ConnectionId connection_id, DisconnectReason reason) = 0;
};

}  // namespace airplaywin::transport
