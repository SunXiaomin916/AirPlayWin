#pragma once

#include <cstdint>

namespace airplaywin::transport {

struct DatagramEndpoint final {
    std::uint32_t ipv4_address_network_order{0U};
    std::uint16_t port{0U};
};

}  // namespace airplaywin::transport
