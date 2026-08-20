#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/transport/DatagramTypes.h"

namespace airplaywin::transport {

class IUdpDatagramHandler {
public:
    virtual ~IUdpDatagramHandler() = default;

    virtual void OnDatagram(std::span<const std::byte> datagram,
                            const DatagramEndpoint& source,
                            std::int64_t arrival_time_nanoseconds) noexcept = 0;
};

}  // namespace airplaywin::transport
