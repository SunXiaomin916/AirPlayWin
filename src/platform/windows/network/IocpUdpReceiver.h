#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "core/transport/IUdpDatagramHandler.h"

namespace airplaywin::windows::network {

struct IocpUdpReceiverOptions final {
    std::string bind_address{"0.0.0.0"};
    std::uint16_t port{0U};
    std::size_t receive_depth{4U};
    std::size_t max_datagram_bytes{8U * 1'024U};
};

struct IocpUdpReceiverDiagnostics final {
    bool running{false};
    std::uint16_t bound_port{0U};
    std::uint64_t received_datagrams{0U};
    std::uint64_t received_bytes{0U};
    std::uint64_t truncated_datagrams{0U};
    std::uint64_t receive_errors{0U};
    std::uint32_t last_error{0U};
};

class IocpUdpReceiver final {
public:
    explicit IocpUdpReceiver(transport::IUdpDatagramHandler& handler);
    ~IocpUdpReceiver();

    IocpUdpReceiver(const IocpUdpReceiver&) = delete;
    IocpUdpReceiver& operator=(const IocpUdpReceiver&) = delete;

    [[nodiscard]] bool Start(const IocpUdpReceiverOptions& options = {});
    void Stop() noexcept;
    [[nodiscard]] bool Running() const noexcept;
    [[nodiscard]] std::uint16_t BoundPort() const noexcept;
    [[nodiscard]] IocpUdpReceiverDiagnostics Diagnostics() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace airplaywin::windows::network
